#define GL_SILENCE_DEPRECATION
#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image.h"
#include "stb_image_write.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_opengl3.h"
#include "imgui.h"
#include "imgui_impl_opengl3_loader.h"
#include <GLFW/glfw3.h>
#include <sqlite3.h>
#include "folder_picker.h"
#include <algorithm>
#include <atomic>
#include <fstream>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#if __APPLE__
#include <mach-o/dyld.h>
#endif

namespace fs = std::filesystem;

// Font Awesome 6 Solid icon codepoints (UTF-8 in string literals for ImGui)
#define ICON_FA_TIMES     "\xef\x80\x8d"
#define ICON_FA_PLAY      "\xef\x81\x8b"
#define ICON_FA_FOLDER_OPEN "\xef\x81\xbc"
#define ICON_FA_PLUS      "\xef\x81\xa7"
#define ICON_FA_FLOPPY    "\xef\x83\x87"
#define ICON_FA_FILM      "\xef\x80\x88"
#define ICON_FA_CHECK     "\xef\x80\x8c"
#define ICON_FA_PEN       "\xef\x8c\x84"
#define ICON_FA_TRASH     "\xef\x8b\xad"
#define ICON_FA_ARROW_UP   "\xef\x81\xa2"
#define ICON_FA_ARROW_DOWN "\xef\x81\xa3"
#define ICON_FA_MINUS     "\xef\x81\xa8"

namespace {

// Returns directory containing the executable, or "." if unavailable.
std::string get_executable_dir() {
#if __APPLE__
  char buf[4096];
  uint32_t size = sizeof(buf);
  if (_NSGetExecutablePath(buf, &size) == 0) {
    std::string path(buf);
    size_t pos = path.find_last_of("/");
    if (pos != std::string::npos)
      return path.substr(0, pos);
  }
#endif
  return ".";
}

struct SqliteDeleter {
  void operator()(sqlite3* p) const {
    if (p) sqlite3_close(p);
  }
};
using SqliteDb = std::unique_ptr<sqlite3, SqliteDeleter>;

struct CurrentProject {
  SqliteDb db;
  std::string path;
  std::string name;
};
CurrentProject g_project;

GLFWwindow* g_main_window = nullptr;
GLFWwindow* g_play_window = nullptr;
double g_play_start_time = 0.;
int g_play_scene_id = 0;
GLuint g_quad_program = 0;

void push_recent_project(const std::string& project_path);
void clear_thumbnail_cache();

std::string get_default_base_path() {
  const char* home = std::getenv("HOME");
  if (!home) home = ".";
  return std::string(home) + "/Documents/chya";
}

bool run_sql(sqlite3* db, const char* sql) {
  char* err = nullptr;
  int r = sqlite3_exec(db, sql, nullptr, nullptr, &err);
  if (r != SQLITE_OK) {
    if (err) sqlite3_free(err);
    return false;
  }
  return true;
}

bool init_schema(sqlite3* db) {
  const char* schema =
      "CREATE TABLE IF NOT EXISTS projects("
      "  id INTEGER PRIMARY KEY, name TEXT NOT NULL, path TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS timeline("
      "  id INTEGER PRIMARY KEY);"
      "CREATE TABLE IF NOT EXISTS scenes("
      "  id INTEGER PRIMARY KEY, timeline_id INTEGER NOT NULL, sort_order INTEGER NOT NULL, name TEXT);"
      "CREATE TABLE IF NOT EXISTS layers("
      "  id INTEGER PRIMARY KEY, scene_id INTEGER NOT NULL, image_path TEXT NOT NULL, sort_order INTEGER NOT NULL);"
      "CREATE TABLE IF NOT EXISTS media("
      "  id INTEGER PRIMARY KEY, path TEXT NOT NULL);"
      "CREATE TABLE IF NOT EXISTS movie_config("
      "  id INTEGER PRIMARY KEY CHECK (id = 1), duration_sec REAL NOT NULL DEFAULT 10,"
      "  frame_rate REAL NOT NULL DEFAULT 24, width INTEGER NOT NULL DEFAULT 1920, height INTEGER NOT NULL DEFAULT 1080);";
  if (!run_sql(db, schema))
    return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM timeline LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  bool has_timeline = sqlite3_step(stmt) == SQLITE_ROW;
  sqlite3_finalize(stmt);
  if (!has_timeline && sqlite3_exec(db, "INSERT INTO timeline(id) VALUES(1)", nullptr, nullptr, nullptr) != SQLITE_OK)
    return false;
  sqlite3_exec(db, "ALTER TABLE scenes ADD COLUMN name TEXT", nullptr, nullptr, nullptr);
  sqlite3_exec(db, "ALTER TABLE layers ADD COLUMN frame_span INTEGER NOT NULL DEFAULT 1", nullptr, nullptr, nullptr);
  stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT 1 FROM movie_config LIMIT 1", -1, &stmt, nullptr) == SQLITE_OK) {
    bool has_config = sqlite3_step(stmt) == SQLITE_ROW;
    sqlite3_finalize(stmt);
    if (!has_config)
      sqlite3_exec(db, "INSERT INTO movie_config(id, duration_sec, frame_rate, width, height) VALUES(1, 10, 24, 1920, 1080)", nullptr, nullptr, nullptr);
  }
  return true;
}

struct MovieConfig {
  double duration_sec = 10.;
  double frame_rate = 24.;
  int width = 1920;
  int height = 1080;
};
MovieConfig get_movie_config(sqlite3* db) {
  MovieConfig c;
  if (!db) return c;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT duration_sec, frame_rate, width, height FROM movie_config WHERE id = 1", -1, &stmt, nullptr) != SQLITE_OK)
    return c;
  if (sqlite3_step(stmt) == SQLITE_ROW) {
    c.duration_sec = sqlite3_column_double(stmt, 0);
    c.frame_rate = sqlite3_column_double(stmt, 1);
    c.width = sqlite3_column_int(stmt, 2);
    c.height = sqlite3_column_int(stmt, 3);
  }
  sqlite3_finalize(stmt);
  return c;
}
bool set_movie_config(sqlite3* db, const MovieConfig& c) {
  if (!db) return false;
  char* sql = sqlite3_mprintf(
      "INSERT INTO movie_config(id, duration_sec, frame_rate, width, height) VALUES(1, %f, %f, %d, %d)"
      " ON CONFLICT(id) DO UPDATE SET duration_sec=excluded.duration_sec, frame_rate=excluded.frame_rate,"
      " width=excluded.width, height=excluded.height",
      c.duration_sec, c.frame_rate, c.width, c.height);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

struct SceneRow {
  int id;
  int sort_order;
  std::string name;
};
std::vector<SceneRow> list_scenes(sqlite3* db) {
  std::vector<SceneRow> out;
  if (!db) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT id, sort_order, COALESCE(name, 'Scene ' || id) FROM scenes ORDER BY sort_order, id", -1, &stmt, nullptr) != SQLITE_OK)
    return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    SceneRow r;
    r.id = sqlite3_column_int(stmt, 0);
    r.sort_order = sqlite3_column_int(stmt, 1);
    const char* n = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 2));
    r.name = n ? n : "";
    out.push_back(r);
  }
  sqlite3_finalize(stmt);
  return out;
}

bool create_scene(sqlite3* db) {
  if (!db) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT COALESCE(MAX(sort_order), 0) + 1 FROM scenes WHERE timeline_id = 1", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  int next_order = 1;
  if (sqlite3_step(stmt) == SQLITE_ROW)
    next_order = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  char* sql = sqlite3_mprintf("INSERT INTO scenes(timeline_id, sort_order, name) VALUES(1, %d, 'Scene %d')", next_order, next_order);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool rename_scene(sqlite3* db, int id, const std::string& name) {
  if (!db || name.empty()) return false;
  char* sql = sqlite3_mprintf("UPDATE scenes SET name = '%q' WHERE id = %d", name.c_str(), id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool delete_scene(sqlite3* db, int id) {
  if (!db) return false;
  char* sql = sqlite3_mprintf("DELETE FROM layers WHERE scene_id = %d; DELETE FROM scenes WHERE id = %d", id, id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool move_scene_up(sqlite3* db, int scene_id) {
  if (!db) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT sort_order FROM scenes WHERE id = ?", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int(stmt, 1, scene_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
  int order = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT id FROM scenes WHERE sort_order < ? ORDER BY sort_order DESC LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int(stmt, 1, order);
  if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
  int other_id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  char* sql = sqlite3_mprintf("UPDATE scenes SET sort_order = CASE id WHEN %d THEN %d WHEN %d THEN %d END WHERE id IN (%d, %d)",
      scene_id, order - 1, other_id, order, scene_id, other_id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool move_scene_down(sqlite3* db, int scene_id) {
  if (!db) return false;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT sort_order FROM scenes WHERE id = ?", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int(stmt, 1, scene_id);
  if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
  int order = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT id FROM scenes WHERE sort_order > ? ORDER BY sort_order ASC LIMIT 1", -1, &stmt, nullptr) != SQLITE_OK)
    return false;
  sqlite3_bind_int(stmt, 1, order);
  if (sqlite3_step(stmt) != SQLITE_ROW) { sqlite3_finalize(stmt); return false; }
  int other_id = sqlite3_column_int(stmt, 0);
  sqlite3_finalize(stmt);
  char* sql = sqlite3_mprintf("UPDATE scenes SET sort_order = CASE id WHEN %d THEN %d WHEN %d THEN %d END WHERE id IN (%d, %d)",
      scene_id, order + 1, other_id, order, scene_id, other_id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

struct LayerRow {
  int id;
  std::string image_path;
  int start_frame;
  int frame_span;
};
std::vector<LayerRow> list_layers(sqlite3* db, int scene_id) {
  std::vector<LayerRow> out;
  if (!db) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT id, image_path, sort_order, COALESCE(frame_span, 1) FROM layers WHERE scene_id = ? ORDER BY sort_order, id", -1, &stmt, nullptr) != SQLITE_OK)
    return out;
  sqlite3_bind_int(stmt, 1, scene_id);
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    LayerRow r;
    r.id = sqlite3_column_int(stmt, 0);
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 1));
    r.image_path = p ? p : "";
    r.start_frame = sqlite3_column_int(stmt, 2);
    r.frame_span = sqlite3_column_int(stmt, 3);
    if (r.frame_span < 1) r.frame_span = 1;
    out.push_back(r);
  }
  sqlite3_finalize(stmt);
  return out;
}

bool add_layer_at_frame(sqlite3* db, int scene_id, int frame_index, const std::string& image_path, int frame_span = 1) {
  if (!db || frame_index < 0 || frame_span < 1) return false;
  char* ins_sql = sqlite3_mprintf("INSERT INTO layers(scene_id, image_path, sort_order, frame_span) VALUES(%d, '%q', %d, %d)", scene_id, image_path.c_str(), frame_index, frame_span);
  bool ok = ins_sql && run_sql(db, ins_sql);
  if (ins_sql) sqlite3_free(ins_sql);
  return ok;
}

bool update_layer_start_frame(sqlite3* db, int layer_id, int start_frame) {
  if (!db || start_frame < 0) return false;
  char* sql = sqlite3_mprintf("UPDATE layers SET sort_order = %d WHERE id = %d", start_frame, layer_id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool update_layer_span(sqlite3* db, int layer_id, int frame_span) {
  if (!db || frame_span < 1) return false;
  char* sql = sqlite3_mprintf("UPDATE layers SET frame_span = %d WHERE id = %d", frame_span, layer_id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool delete_layer(sqlite3* db, int layer_id) {
  if (!db) return false;
  char* sql = sqlite3_mprintf("DELETE FROM layers WHERE id = %d", layer_id);
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

std::string get_image_at_frame(sqlite3* db, int scene_id, int frame) {
  std::vector<LayerRow> layers = list_layers(db, scene_id);
  std::string path;
  for (const LayerRow& L : layers)
    if (frame >= L.start_frame && frame < L.start_frame + L.frame_span)
      path = L.image_path;
  return path;
}

int get_scene_used_frame_count(sqlite3* db, int scene_id) {
  std::vector<LayerRow> layers = list_layers(db, scene_id);
  int end_frame = 0;
  for (const LayerRow& L : layers) {
    int layer_end = L.start_frame + L.frame_span;
    if (layer_end > end_frame) end_frame = layer_end;
  }
  return end_frame;
}

static void scale_rgba_to(const unsigned char* src, int sw, int sh, unsigned char* dst, int dw, int dh) {
  for (int y = 0; y < dh; y++) {
    int sy = (dh > 1 && sh > 0) ? (y * (sh - 1) / (dh - 1)) : 0;
    if (sy >= sh) sy = sh - 1;
    for (int x = 0; x < dw; x++) {
      int sx = (dw > 1 && sw > 0) ? (x * (sw - 1) / (dw - 1)) : 0;
      if (sx >= sw) sx = sw - 1;
      int si = (sy * sw + sx) * 4;
      int di = (y * dw + x) * 4;
      dst[di] = src[si];
      dst[di + 1] = src[si + 1];
      dst[di + 2] = src[si + 2];
      dst[di + 3] = src[si + 3];
    }
  }
}

bool render_project_to_video(sqlite3* db, const std::string& project_root, const std::string& output_path, std::atomic<float>* progress) {
  if (!db || project_root.empty() || output_path.empty()) return false;
  MovieConfig cfg = get_movie_config(db);
  std::vector<SceneRow> scenes = list_scenes(db);
  if (scenes.empty()) return false;
  int total_frames = 0;
  std::vector<int> scene_frame_counts;
  scene_frame_counts.reserve(scenes.size());
  for (const SceneRow& scene : scenes) {
    int n = get_scene_used_frame_count(db, scene.id);
    scene_frame_counts.push_back(n);
    total_frames += n;
  }
  if (total_frames <= 0) return false;
  const int out_w = cfg.width;
  const int out_h = cfg.height;
  fs::path tmp_dir = fs::path(project_root) / ".render_frames";
  std::error_code ec;
  fs::create_directories(tmp_dir, ec);
  if (ec) return false;
  std::vector<std::string> written;
  int frame_idx = 0;
  std::vector<unsigned char> out_buf(static_cast<size_t>(out_w) * out_h * 4, 0);
  for (size_t s = 0; s < scenes.size(); s++) {
    const SceneRow& scene = scenes[s];
    const int frames_this_scene = scene_frame_counts[s];
    for (int f = 0; f < frames_this_scene; f++) {
      if (progress) progress->store(static_cast<float>(frame_idx) / static_cast<float>(total_frames));
      std::string rel = get_image_at_frame(db, scene.id, f);
      if (!rel.empty()) {
        std::string full = (fs::path(project_root) / rel).string();
        int iw = 0, ih = 0, ic = 0;
        unsigned char* img = stbi_load(full.c_str(), &iw, &ih, &ic, 4);
        if (img && iw > 0 && ih > 0) {
          scale_rgba_to(img, iw, ih, out_buf.data(), out_w, out_h);
          stbi_image_free(img);
        } else {
          std::fill(out_buf.begin(), out_buf.end(), 0);
        }
      } else {
        std::fill(out_buf.begin(), out_buf.end(), 0);
      }
      char fn[256];
      snprintf(fn, sizeof(fn), "frame_%05d.png", frame_idx);
      std::string path = (tmp_dir / fn).string();
      if (!stbi_write_png(path.c_str(), out_w, out_h, 4, out_buf.data(), 0))
        break;
      written.push_back(path);
      frame_idx++;
    }
  }
  if (progress) progress->store(1.f);
  if (written.size() != static_cast<size_t>(frame_idx)) {
    for (const auto& p : written) fs::remove(p, ec);
    fs::remove(tmp_dir, ec);
    return false;
  }
  std::string ff_cmd = "ffmpeg -y -framerate " + std::to_string(static_cast<int>(cfg.frame_rate)) +
      " -i \"" + tmp_dir.string() + "/frame_%05d.png\" -c:v libx264 -pix_fmt yuv420p \"" + output_path + "\" 2>/dev/null";
  int ret = std::system(ff_cmd.c_str());
  for (const auto& p : written) fs::remove(p, ec);
  fs::remove(tmp_dir, ec);
  return (ret == 0);
}

static void render_worker(std::string project_root, std::string output_path, std::atomic<float>* progress, std::atomic<int>* done) {
  sqlite3* db = nullptr;
  if (sqlite3_open((project_root + "/project.db").c_str(), &db) != SQLITE_OK) {
    if (done) done->store(-1);
    return;
  }
  bool ok = render_project_to_video(db, project_root, output_path, progress);
  sqlite3_close(db);
  if (done) done->store(ok ? 1 : -1);
  if (progress) progress->store(1.f);
}

void close_project() {
  if (g_play_window) {
    glfwDestroyWindow(g_play_window);
    g_play_window = nullptr;
  }
  clear_thumbnail_cache();
  g_project.db.reset();
  g_project.path.clear();
  g_project.name.clear();
}

bool open_project_db(const std::string& project_root, const std::string& project_name) {
  close_project();
  std::string db_path = project_root + "/project.db";
  sqlite3* raw = nullptr;
  if (sqlite3_open(db_path.c_str(), &raw) != SQLITE_OK)
    return false;
  if (!init_schema(raw)) {
    sqlite3_close(raw);
    return false;
  }
  g_project.db.reset(raw);
  g_project.path = project_root;
  g_project.name = project_name;
  push_recent_project(project_root);
  return true;
}

std::string sanitize_project_name(std::string s) {
  for (char& c : s) {
    if (c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
      c = '_';
  }
  while (!s.empty() && (s.back() == ' ' || s.back() == '.'))
    s.pop_back();
  if (s.empty()) s = "Untitled";
  return s;
}

bool create_project(const std::string& name) {
  std::string base = get_default_base_path();
  fs::create_directories(base);
  std::string safe_name = sanitize_project_name(name);
  fs::path project_root = fs::path(base) / safe_name;
  std::error_code ec;
  if (!fs::create_directories(project_root, ec) && !fs::exists(project_root))
    return false;
  fs::create_directories(project_root / "media", ec);
  std::string db_path = (project_root / "project.db").string();
  sqlite3* raw = nullptr;
  if (sqlite3_open(db_path.c_str(), &raw) != SQLITE_OK)
    return false;
  if (!init_schema(raw)) {
    sqlite3_close(raw);
    return false;
  }
  char* insert_sql = sqlite3_mprintf(
      "INSERT INTO projects(name, path) VALUES('%q', '%q')",
      safe_name.c_str(), project_root.string().c_str());
  bool ok = insert_sql && run_sql(raw, insert_sql);
  if (insert_sql) sqlite3_free(insert_sql);
  if (!ok) {
    sqlite3_close(raw);
    return false;
  }
  g_project.db.reset(raw);
  g_project.path = project_root.string();
  g_project.name = safe_name;
  push_recent_project(g_project.path);
  return true;
}

std::string get_recent_file_path() {
  return get_default_base_path() + "/recent.txt";
}

constexpr size_t kMaxRecentProjects = 10;

std::vector<std::string> load_recent_projects() {
  std::vector<std::string> out;
  std::ifstream f(get_recent_file_path());
  std::string line;
  while (out.size() < kMaxRecentProjects && std::getline(f, line)) {
    while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
      line.pop_back();
    if (!line.empty()) out.push_back(line);
  }
  return out;
}

void push_recent_project(const std::string& project_path) {
  std::vector<std::string> recent = load_recent_projects();
  recent.erase(std::remove(recent.begin(), recent.end(), project_path), recent.end());
  recent.insert(recent.begin(), project_path);
  if (recent.size() > kMaxRecentProjects) recent.resize(kMaxRecentProjects);
  std::string path = get_recent_file_path();
  std::error_code ec;
  fs::create_directories(fs::path(path).parent_path(), ec);
  std::ofstream out(path);
  for (const auto& p : recent) out << p << '\n';
}

std::vector<std::string> list_project_folders() {
  std::vector<std::string> out;
  std::string base = get_default_base_path();
  if (!fs::exists(base)) return out;
  for (const auto& e : fs::directory_iterator(base)) {
    if (!e.is_directory()) continue;
    if (fs::exists(e.path() / "project.db"))
      out.push_back(e.path().string());
  }
  return out;
}

bool is_image_extension(const std::string& path) {
  std::string ext = fs::path(path).extension().string();
  std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
  return ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" ||
         ext == ".bmp" || ext == ".webp" || ext == ".tga";
}

bool add_media_file(sqlite3* db, const std::string& project_root, const std::string& source_path) {
  if (!db || project_root.empty() || !fs::is_regular_file(source_path))
    return false;
  fs::path dest_dir = fs::path(project_root) / "media";
  std::string stem = fs::path(source_path).stem().string();
  std::string ext = fs::path(source_path).extension().string();
  fs::path dest = dest_dir / (stem + ext);
  int n = 0;
  while (fs::exists(dest))
    dest = dest_dir / (stem + "_" + std::to_string(++n) + ext);
  std::error_code ec;
  fs::copy(fs::path(source_path), dest, fs::copy_options::overwrite_existing, ec);
  if (ec) return false;
  std::string rel = "media/" + dest.filename().string();
  char* sql = sqlite3_mprintf("INSERT INTO media(path) VALUES('%q')", rel.c_str());
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

std::vector<std::string> list_media(sqlite3* db) {
  std::vector<std::string> out;
  if (!db) return out;
  sqlite3_stmt* stmt = nullptr;
  if (sqlite3_prepare_v2(db, "SELECT path FROM media ORDER BY id", -1, &stmt, nullptr) != SQLITE_OK)
    return out;
  while (sqlite3_step(stmt) == SQLITE_ROW) {
    const char* p = reinterpret_cast<const char*>(sqlite3_column_text(stmt, 0));
    if (p) out.push_back(p);
  }
  sqlite3_finalize(stmt);
  return out;
}

bool delete_media(sqlite3* db, const std::string& rel_path) {
  if (!db || rel_path.empty()) return false;
  char* sql = sqlite3_mprintf("DELETE FROM media WHERE path = '%q'", rel_path.c_str());
  bool ok = sql && run_sql(db, sql);
  if (sql) sqlite3_free(sql);
  return ok;
}

bool rename_media(sqlite3* db, const std::string& project_root, const std::string& old_rel_path, const std::string& new_filename) {
  if (!db || project_root.empty() || old_rel_path.empty() || new_filename.empty())
    return false;
  if (old_rel_path.find("media/") != 0) return false;
  std::string new_rel = "media/" + new_filename;
  if (new_rel == old_rel_path) return true;
  fs::path old_full = fs::path(project_root) / old_rel_path;
  fs::path new_full = fs::path(project_root) / new_rel;
  if (!fs::is_regular_file(old_full)) return false;
  if (fs::exists(new_full)) return false;
  std::error_code ec;
  fs::rename(old_full, new_full, ec);
  if (ec) return false;
  char* sql_media = sqlite3_mprintf("UPDATE media SET path = '%q' WHERE path = '%q'", new_rel.c_str(), old_rel_path.c_str());
  bool ok = sql_media && run_sql(db, sql_media);
  if (sql_media) sqlite3_free(sql_media);
  if (!ok) return false;
  char* sql_layers = sqlite3_mprintf("UPDATE layers SET image_path = '%q' WHERE image_path = '%q'", new_rel.c_str(), old_rel_path.c_str());
  ok = sql_layers && run_sql(db, sql_layers);
  if (sql_layers) sqlite3_free(sql_layers);
  return ok;
}

std::vector<std::string> g_dropped_paths;

constexpr int kThumbSize = 80;
struct ThumbEntry { GLuint tex = 0; int w = 0; int h = 0; };
std::map<std::string, ThumbEntry> g_thumb_cache;

void clear_thumbnail_cache() {
  for (auto& p : g_thumb_cache) {
    if (p.second.tex != 0)
      glDeleteTextures(1, &p.second.tex);
  }
  g_thumb_cache.clear();
}

ImTextureID get_thumbnail_texture(const std::string& project_root, const std::string& rel_path, int* out_w = nullptr, int* out_h = nullptr) {
  std::string key = project_root + "/" + rel_path;
  auto it = g_thumb_cache.find(key);
  if (it != g_thumb_cache.end()) {
    if (out_w) *out_w = it->second.w;
    if (out_h) *out_h = it->second.h;
    return (ImTextureID)(intptr_t)it->second.tex;
  }
  std::string full = (fs::path(project_root) / rel_path).string();
  int w = 0, h = 0, comp = 0;
  unsigned char* data = stbi_load(full.c_str(), &w, &h, &comp, 4);
  if (!data || w <= 0 || h <= 0) return nullptr;
  GLuint tex = 0;
  glGenTextures(1, &tex);
  glBindTexture(GL_TEXTURE_2D, tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  stbi_image_free(data);
  glBindTexture(GL_TEXTURE_2D, 0);
  g_thumb_cache[key] = ThumbEntry{tex, w, h};
  if (out_w) *out_w = w;
  if (out_h) *out_h = h;
  return (ImTextureID)(intptr_t)tex;
}

void drop_callback(GLFWwindow*, int count, const char** paths) {
  for (int i = 0; i < count; i++)
    if (paths[i]) g_dropped_paths.push_back(paths[i]);
}

constexpr int kWindowWidth = 1280;
constexpr int kWindowHeight = 720;
constexpr float kClearColorR = 0.12f;
constexpr float kClearColorG = 0.12f;
constexpr float kClearColorB = 0.14f;
constexpr float kClearColorA = 1.f;

#ifndef GL_STATIC_DRAW
#define GL_STATIC_DRAW 0x88E4
#endif
typedef void (*PFNGLDRAWARRAYSPROC)(unsigned int mode, int first, int count);

static const char* kQuadVs = "#version 330\n"
  "layout(location=0) in vec2 pos;\n"
  "out vec2 uv;\n"
  "void main() { gl_Position = vec4(pos, 0, 1); uv = pos*0.5+0.5; }\n";
static const char* kQuadFs = "#version 330\n"
  "in vec2 uv; uniform sampler2D tex; out vec4 fragColor;\n"
  "void main() { fragColor = texture(tex, vec2(uv.x, 1.0 - uv.y)); }\n";

GLuint create_quad_program() {
  GLuint vs = glCreateShader(GL_VERTEX_SHADER);
  glShaderSource(vs, 1, &kQuadVs, nullptr);
  glCompileShader(vs);
  GLuint fs = glCreateShader(GL_FRAGMENT_SHADER);
  glShaderSource(fs, 1, &kQuadFs, nullptr);
  glCompileShader(fs);
  GLuint prog = glCreateProgram();
  glAttachShader(prog, vs);
  glAttachShader(prog, fs);
  glLinkProgram(prog);
  glDeleteShader(vs);
  glDeleteShader(fs);
  return prog;
}

void draw_textured_quad(GLuint program, GLuint texture_id) {
  static GLuint vao = 0, vbo = 0;
  static PFNGLDRAWARRAYSPROC fn_draw_arrays = nullptr;
  if (vao == 0) {
    float verts[] = {-1,-1, 1,-1, -1,1,  -1,1, 1,-1, 1,1};
    glGenVertexArrays(1, &vao);
    glGenBuffers(1, &vbo);
    glBindVertexArray(vao);
    glBindBuffer(GL_ARRAY_BUFFER, vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(verts), verts, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    glBindVertexArray(0);
  }
  if (fn_draw_arrays == nullptr && g_play_window)
    fn_draw_arrays = (PFNGLDRAWARRAYSPROC)glfwGetProcAddress("glDrawArrays");
  if (fn_draw_arrays == nullptr) return;
  glUseProgram(program);
  glActiveTexture(GL_TEXTURE0);
  glBindTexture(GL_TEXTURE_2D, texture_id);
  glUniform1i(glGetUniformLocation(program, "tex"), 0);
  glBindVertexArray(vao);
  fn_draw_arrays(GL_TRIANGLES, 0, 6);
  glBindVertexArray(0);
}

void set_glfw_window_hints() {
  glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
  glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
  glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
#if __APPLE__
  glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GL_TRUE);
#endif
}

GLFWwindow* create_app_window() {
  set_glfw_window_hints();
  GLFWwindow* window =
      glfwCreateWindow(kWindowWidth, kWindowHeight, "chya", nullptr, nullptr);
  if (window) {
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1);
  }
  return window;
}

void init_imgui(GLFWwindow* window) {
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGuiIO& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
  ImGui::StyleColorsDark();

  // Merge Font Awesome 6 Solid so icons can be used in button labels
  io.Fonts->AddFontDefault();
  std::string fa_path = get_executable_dir() + "/fa-solid-900.ttf";
  if (!fs::exists(fa_path))
    fa_path = "fa-solid-900.ttf";
  if (fs::exists(fa_path)) {
    static const ImWchar fa_ranges[] = { 0xf008, 0xf308, 0 };
    ImFontConfig cfg;
    cfg.MergeMode = true;
    cfg.GlyphOffset.y = 1.f;
    io.Fonts->AddFontFromFileTTF(fa_path.c_str(), 14.f, &cfg, fa_ranges);
  }

  ImGui_ImplGlfw_InitForOpenGL(window, true);
  ImGui_ImplOpenGL3_Init("#version 330");
}

void shutdown_imgui() {
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
}

void begin_frame() {
  ImGui_ImplOpenGL3_NewFrame();
  ImGui_ImplGlfw_NewFrame();
  ImGui::NewFrame();
}

enum class AppModal { None, CreateProject, OpenProject };
AppModal g_modal = AppModal::None;
char g_new_project_name[256] = "";

ImTextureID get_logo_texture() {
  static GLuint logo_tex = 0;
  if (logo_tex != 0) return (ImTextureID)(intptr_t)logo_tex;
  std::string path = get_executable_dir() + "/logo.png";
  int w = 0, h = 0, comp = 0;
  unsigned char* data = stbi_load(path.c_str(), &w, &h, &comp, 4);
  if (!data || w <= 0 || h <= 0) return nullptr;
  glGenTextures(1, &logo_tex);
  glBindTexture(GL_TEXTURE_2D, logo_tex);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, data);
  stbi_image_free(data);
  glBindTexture(GL_TEXTURE_2D, 0);
  return (ImTextureID)(intptr_t)logo_tex;
}

void draw_center_create_or_open() {
  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImVec2 center(vp->WorkPos.x + vp->WorkSize.x * 0.5f, vp->WorkPos.y + vp->WorkSize.y * 0.5f);
  ImGui::SetNextWindowPos(center, ImGuiCond_Always, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(320, 280), ImVec2(500, 600));
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_AlwaysAutoResize;
  if (ImGui::Begin("Create or open project", nullptr, flags)) {
    ImTextureID logo = get_logo_texture();
    if (logo) {
      const float logo_sz = 96.f;
      ImVec2 logo_size(logo_sz, logo_sz);
      float off = (ImGui::GetContentRegionAvail().x - logo_sz) * 0.5f;
      if (off > 0.f) ImGui::SetCursorPosX(ImGui::GetCursorPosX() + off);
      ImGui::Image(logo, logo_size);
      ImGui::Spacing();
    }
    if (ImGui::Button(ICON_FA_PLUS " Create project", ImVec2(200, 0))) {
      g_modal = AppModal::CreateProject;
      g_new_project_name[0] = '\0';
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Open project", ImVec2(200, 0))) {
      static char picked_path[4096] = "";
      if (pick_project_folder(picked_path, sizeof(picked_path))) {
        std::string path(picked_path);
        if (fs::exists(fs::path(path) / "project.db")) {
          std::string label = fs::path(path).filename().string();
          open_project_db(path, label);
        }
      } else {
        ImGui::OpenPopup("Open project");
        g_modal = AppModal::OpenProject;
      }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Recently opened projects");
    std::vector<std::string> recent = load_recent_projects();
    if (ImGui::BeginChild("##recent_list", ImVec2(-1, 140), true, ImGuiWindowFlags_None)) {
      bool any = false;
      for (const std::string& path : recent) {
        if (!fs::exists(fs::path(path) / "project.db")) continue;
        any = true;
        std::string label = fs::path(path).filename().string();
        if (ImGui::Selectable(label.c_str())) {
          open_project_db(path, label);
        }
      }
      if (!any)
        ImGui::TextDisabled("(No recent projects)");
    }
    ImGui::EndChild();
  }
  ImGui::End();
}

void draw_window_create_project() {
  bool open = true;
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  ImGui::SetNextWindowSize(ImVec2(360, 0), ImGuiCond_FirstUseEver);
  if (ImGui::Begin("New project", &open, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Project name:");
    ImGui::SetNextItemWidth(-1);
    ImGui::InputText("##name", g_new_project_name, sizeof(g_new_project_name));
    if (ImGui::Button(ICON_FA_CHECK " Create", ImVec2(120, 0))) {
      if (create_project(g_new_project_name)) {
        g_new_project_name[0] = '\0';
        open = false;
        g_modal = AppModal::None;
      }
    }
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_TIMES " Cancel", ImVec2(120, 0))) {
      g_new_project_name[0] = '\0';
      open = false;
      g_modal = AppModal::None;
    }
  }
  ImGui::End();
  if (!open)
    g_modal = AppModal::None;
}

void draw_modal_open_project() {
  ImVec2 center = ImGui::GetMainViewport()->GetCenter();
  ImGui::SetNextWindowPos(center, ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
  if (ImGui::BeginPopupModal("Open project", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    static char picked_path[4096] = "";
    if (ImGui::Button(ICON_FA_FOLDER_OPEN " Browse for folder...", ImVec2(200, 0))) {
      if (pick_project_folder(picked_path, sizeof(picked_path))) {
        std::string path(picked_path);
        if (fs::exists(fs::path(path) / "project.db")) {
          std::string label = fs::path(path).filename().string();
          if (open_project_db(path, label)) {
            picked_path[0] = '\0';
            ImGui::CloseCurrentPopup();
            g_modal = AppModal::None;
          }
        }
      }
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Projects in %s", get_default_base_path().c_str());
    std::vector<std::string> folders = list_project_folders();
    if (folders.empty()) {
      ImGui::Text("(none)");
    } else {
      for (const std::string& p : folders) {
        std::string label = fs::path(p).filename().string();
        if (ImGui::Button(label.c_str(), ImVec2(300, 0))) {
          if (open_project_db(p, label)) {
            ImGui::CloseCurrentPopup();
            g_modal = AppModal::None;
          }
        }
      }
    }
    if (ImGui::Button(ICON_FA_TIMES " Close")) {
      ImGui::CloseCurrentPopup();
      g_modal = AppModal::None;
    }
    ImGui::EndPopup();
  }
}

void draw_ui() {
  if (!g_project.db) {
    g_dropped_paths.clear();
    draw_center_create_or_open();
    if (g_modal == AppModal::CreateProject)
      draw_window_create_project();
    else if (g_modal == AppModal::OpenProject)
      draw_modal_open_project();
    return;
  }

  for (const std::string& p : g_dropped_paths) {
    if (is_image_extension(p))
      add_media_file(g_project.db.get(), g_project.path, p);
  }
  g_dropped_paths.clear();

  ImGuiViewport* vp = ImGui::GetMainViewport();
  ImGui::SetNextWindowPos(vp->WorkPos);
  ImGui::SetNextWindowSize(vp->WorkSize);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                           | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoCollapse;
  static int s_rename_scene_id = 0;
  static char s_rename_buf[256] = "";
  static bool s_open_rename_popup = false;
  static bool s_open_rename_media_popup = false;
  static char s_rename_media_buf[256] = "";
  static int s_selected_scene_id = 0;
  static int s_selected_layer_id = 0;
  static int s_selected_layer_scene_id = 0;
  static std::string s_selected_media_path;
  static int s_pixels_per_frame = 8;
  static std::string s_clipboard_path;
  static int s_clipboard_frame_span = 1;
  static std::atomic<float> s_render_progress(-1.f);
  static std::atomic<int> s_render_done(0);
  static std::unique_ptr<std::thread> s_render_thread;
  static ImVec2 s_render_btn_min(0, 0), s_render_btn_max(0, 0);
  static bool s_render_btn_rect_valid = false;

  if (ImGui::Begin("##project_root", nullptr, flags)) {
    if (s_open_rename_popup) {
      ImGui::OpenPopup("Rename scene");
      s_open_rename_popup = false;
    }
    if (s_open_rename_media_popup) {
      ImGui::OpenPopup("Rename media");
      s_open_rename_media_popup = false;
    }
    if (ImGui::Button(ICON_FA_TIMES " Close project"))
      close_project();
    ImGui::SameLine();
    ImGui::Text("Project: %s", g_project.name.c_str());
    const float play_btn_w = 72.f;
    const float render_btn_w = 88.f;
    ImGui::SameLine(ImGui::GetWindowWidth() - play_btn_w - render_btn_w - ImGui::GetStyle().ItemSpacing.x - ImGui::GetStyle().WindowPadding.x);

    if (s_render_done.load() != 0) {
      if (s_render_thread && s_render_thread->joinable())
        s_render_thread->join();
      s_render_thread.reset();
      if (s_render_done.load() == 1)
        ImGui::OpenPopup("##render_ok");
      else
        ImGui::OpenPopup("##render_fail");
      s_render_done.store(0);
      s_render_progress.store(-1.f);
    }

    float prog = s_render_progress.load();
    const bool rendering = (prog >= 0.f && prog < 1.f);
    if (rendering && s_render_btn_rect_valid) {
      ImDrawList* dl = ImGui::GetWindowDrawList();
      float w = s_render_btn_max.x - s_render_btn_min.x;
      ImVec2 fill_max(s_render_btn_min.x + w * prog, s_render_btn_max.y);
      dl->AddRectFilled(s_render_btn_min, s_render_btn_max, IM_COL32(40, 40, 45, 255));
      if (fill_max.x > s_render_btn_min.x)
        dl->AddRectFilled(s_render_btn_min, fill_max, IM_COL32(70, 120, 180, 255));
    }

    if (rendering)
      ImGui::BeginDisabled();
    if (ImGui::Button(rendering ? ICON_FA_FILM " Render..." : ICON_FA_FILM " Render", ImVec2(render_btn_w, 0))) {
      if (!rendering && g_project.db && !g_project.path.empty() && !list_scenes(g_project.db.get()).empty()) {
        static char save_path[4096] = "";
        if (pick_save_file(save_path, sizeof(save_path), "output.mp4")) {
          s_render_progress.store(0.f);
          s_render_done.store(0);
          s_render_thread = std::make_unique<std::thread>(render_worker, g_project.path, std::string(save_path), &s_render_progress, &s_render_done);
        }
      }
    }
    if (rendering)
      ImGui::EndDisabled();
    if (prog >= 0.f && prog <= 1.f) {
      s_render_btn_min = ImGui::GetItemRectMin();
      s_render_btn_max = ImGui::GetItemRectMax();
      s_render_btn_rect_valid = true;
    } else {
      s_render_btn_rect_valid = false;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip(rendering ? "Rendering..." : "Render all scenes to video (requires ffmpeg)");
    ImGui::SameLine();
    if (ImGui::Button(ICON_FA_PLAY " Play", ImVec2(play_btn_w, 0))) {
      if (g_play_window) {
        glfwFocusWindow(g_play_window);
      } else if (s_selected_scene_id != 0 && g_project.db && g_main_window) {
        set_glfw_window_hints();
        g_play_window = glfwCreateWindow(640, 360, "Timeline playback", nullptr, g_main_window);
        if (g_play_window) {
          g_play_start_time = glfwGetTime();
          g_play_scene_id = s_selected_scene_id;
        }
      }
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Play timeline in separate window");
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##render_ok", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Video saved successfully.");
      if (ImGui::Button(ICON_FA_CHECK " OK")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("##render_fail", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::Text("Render failed. Is ffmpeg installed?");
      if (ImGui::Button(ICON_FA_CHECK " OK")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
    ImGui::Separator();

    if (!ImGui::IsAnyItemActive() && (ImGui::IsKeyPressed(ImGuiKey_Delete) || ImGui::IsKeyPressed(ImGuiKey_Backspace))) {
      if (s_selected_layer_id != 0) {
        delete_layer(g_project.db.get(), s_selected_layer_id);
        s_selected_layer_id = 0;
      } else if (!s_selected_media_path.empty()) {
        if (delete_media(g_project.db.get(), s_selected_media_path)) {
          std::string key = g_project.path + "/" + s_selected_media_path;
          auto it = g_thumb_cache.find(key);
          if (it != g_thumb_cache.end()) {
            if (it->second.tex != 0) glDeleteTextures(1, &it->second.tex);
            g_thumb_cache.erase(it);
          }
          s_selected_media_path.clear();
        }
      }
    }

    const float avail_y = ImGui::GetContentRegionAvail().y;
    const float timeline_h = (s_selected_scene_id != 0) ? (avail_y * 0.5f) : 0.f;
    const float top_h = (s_selected_scene_id != 0) ? (-timeline_h - ImGui::GetStyle().ItemSpacing.y) : -1.f;

    if (!ImGui::BeginChild("##top_panels", ImVec2(0, top_h), false, ImGuiWindowFlags_None)) {
      ImGui::EndChild();
      ImGui::End();
      ImGui::PopStyleVar(2);
      return;
    }

    const float config_w = 220.f;
    const float gap = ImGui::GetStyle().ItemSpacing.x * 2.f;
    const float rest = std::max(0.f, ImGui::GetContentRegionAvail().x - config_w - gap);
    const float media_w = rest * 0.5f;

    if (ImGui::BeginChild("##config_panel", ImVec2(config_w, -1), true, ImGuiWindowFlags_None)) {
      ImGui::Text("Configuration");
      ImGui::Spacing();
      MovieConfig cfg = get_movie_config(g_project.db.get());
      bool changed = false;
      ImGui::Text("Duration (sec)");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputDouble("##duration", &cfg.duration_sec, 0.5, 1.0, "%.1f", ImGuiInputTextFlags_None))
        changed = true;
      if (cfg.duration_sec < 0.1) cfg.duration_sec = 0.1;
      ImGui::Text("Frame rate (fps)");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputDouble("##fps", &cfg.frame_rate, 1.0, 5.0, "%.1f", ImGuiInputTextFlags_None))
        changed = true;
      if (cfg.frame_rate < 1.0) cfg.frame_rate = 1.0;
      ImGui::Text("Width");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputInt("##width", &cfg.width, 1, 100, ImGuiInputTextFlags_None))
        changed = true;
      if (cfg.width < 1) cfg.width = 1;
      if (cfg.width > 7680) cfg.width = 7680;
      ImGui::Text("Height");
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputInt("##height", &cfg.height, 1, 100, ImGuiInputTextFlags_None))
        changed = true;
      if (cfg.height < 1) cfg.height = 1;
      if (cfg.height > 4320) cfg.height = 4320;
      if (changed)
        set_movie_config(g_project.db.get(), cfg);
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##media_panel", ImVec2(media_w, -1), true, ImGuiWindowFlags_None)) {
      ImGui::Text("Media");
      ImGui::Text("Drop images onto the window to add to project.");
      if (!s_selected_media_path.empty()) {
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PEN " Rename")) {
          std::string fname = fs::path(s_selected_media_path).filename().string();
          strncpy(s_rename_media_buf, fname.c_str(), sizeof(s_rename_media_buf) - 1);
          s_rename_media_buf[sizeof(s_rename_media_buf) - 1] = '\0';
          s_open_rename_media_popup = true;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Rename selected media file");
      }
      ImGui::Spacing();
      std::vector<std::string> media = list_media(g_project.db.get());
      const float thumb_sz = static_cast<float>(kThumbSize);
      const float spacing = ImGui::GetStyle().ItemSpacing.x;
      const int cols = (thumb_sz + spacing > 0) ? std::max(1, static_cast<int>(ImGui::GetContentRegionAvail().x / (thumb_sz + spacing))) : 1;
      int col = 0;
      for (const std::string& rel : media) {
        ImGui::PushID(rel.c_str());
        ImTextureID tex = get_thumbnail_texture(g_project.path, rel);
        if (tex) {
          ImGui::Image(tex, ImVec2(thumb_sz, thumb_sz));
          if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
            ImGui::SetDragDropPayload("CHYA_MEDIA", rel.c_str(), rel.size() + 1);
            ImGui::Text("%s", rel.c_str());
            ImGui::EndDragDropSource();
          }
        } else {
          ImGui::Dummy(ImVec2(thumb_sz, thumb_sz));
        }
        if (ImGui::IsItemClicked(0)) {
          s_selected_media_path = rel;
          s_selected_layer_id = 0;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("%s (drag to timeline, Delete to remove)", rel.c_str());
        if (s_selected_media_path == rel) {
          ImVec2 a = ImGui::GetItemRectMin();
          ImVec2 b = ImGui::GetItemRectMax();
          ImGui::GetWindowDrawList()->AddRect(a, b, IM_COL32(255, 255, 0, 255), 0.f, 0, 3.f);
        }
        ImGui::PopID();
        col++;
        if (col < cols)
          ImGui::SameLine();
        else
          col = 0;
      }
    }
    ImGui::EndChild();

    ImGui::SameLine();
    if (ImGui::BeginChild("##scenes_panel", ImVec2(rest - media_w, -1), true, ImGuiWindowFlags_None)) {
      ImGui::Text("Scenes");
      if (ImGui::Button(ICON_FA_PLUS " New scene"))
        create_scene(g_project.db.get());
      ImGui::Spacing();
      std::vector<SceneRow> scenes_list = list_scenes(g_project.db.get());
      const float btn_sz = 22.f;
      const float spacing = ImGui::GetStyle().ItemSpacing.x;
      const float buttons_w = btn_sz * 4.f + spacing * 3.f;
      const float panel_w = ImGui::GetWindowWidth() - ImGui::GetStyle().WindowPadding.x * 2.f;
      const float scrollbar_w = ImGui::GetStyle().ScrollbarSize;
      const float max_row_w = panel_w - scrollbar_w;
      for (size_t i = 0; i < scenes_list.size(); i++) {
        const SceneRow& scene = scenes_list[i];
        ImGui::PushID(scene.id);
        ImGui::AlignTextToFramePadding();
        const float avail_x = std::min(ImGui::GetContentRegionAvail().x, max_row_w);
        const float selectable_w = std::max(0.f, avail_x - buttons_w);
        if (ImGui::Selectable(scene.name.c_str(), s_selected_scene_id == scene.id, 0, ImVec2(selectable_w, 0)))
          s_selected_scene_id = scene.id;
        ImGui::SameLine(ImGui::GetCursorPosX() + selectable_w + spacing);
        if (ImGui::Button(ICON_FA_ARROW_UP, ImVec2(btn_sz, 0)) && i > 0)
          move_scene_up(g_project.db.get(), scene.id);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Move up");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_ARROW_DOWN, ImVec2(btn_sz, 0)) && i + 1 < scenes_list.size())
          move_scene_down(g_project.db.get(), scene.id);
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Move down");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PEN, ImVec2(btn_sz, 0))) {
          s_rename_scene_id = scene.id;
          strncpy(s_rename_buf, scene.name.c_str(), sizeof(s_rename_buf) - 1);
          s_rename_buf[sizeof(s_rename_buf) - 1] = '\0';
          s_open_rename_popup = true;
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Rename");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_TRASH, ImVec2(btn_sz, 0))) {
          if (s_selected_scene_id == scene.id) s_selected_scene_id = 0;
          delete_scene(g_project.db.get(), scene.id);
        }
        if (ImGui::IsItemHovered())
          ImGui::SetTooltip("Delete");
        ImGui::PopID();
      }
    }
    ImGui::EndChild();

    ImGui::EndChild();

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename scene", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::SetNextItemWidth(240);
      ImGui::InputText("##name", s_rename_buf, sizeof(s_rename_buf));
      if (ImGui::Button(ICON_FA_CHECK " OK", ImVec2(80, 0))) {
        if (rename_scene(g_project.db.get(), s_rename_scene_id, s_rename_buf)) {
          s_rename_scene_id = 0;
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::SameLine();
if (ImGui::Button(ICON_FA_TIMES " Cancel", ImVec2(80, 0))) {
        s_rename_scene_id = 0;
          ImGui::CloseCurrentPopup();
        }
      ImGui::EndPopup();
    }

    ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    if (ImGui::BeginPopupModal("Rename media", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::SetNextItemWidth(280);
      ImGui::InputText("##media_name", s_rename_media_buf, sizeof(s_rename_media_buf));
      if (ImGui::Button(ICON_FA_CHECK " OK", ImVec2(80, 0))) {
        std::string new_name(s_rename_media_buf);
        while (!new_name.empty() && (new_name.back() == ' ' || new_name.back() == '\n')) new_name.pop_back();
        if (!new_name.empty() && rename_media(g_project.db.get(), g_project.path, s_selected_media_path, new_name)) {
          std::string old_key = g_project.path + "/" + s_selected_media_path;
          auto it = g_thumb_cache.find(old_key);
          if (it != g_thumb_cache.end()) {
            if (it->second.tex != 0) glDeleteTextures(1, &it->second.tex);
            g_thumb_cache.erase(it);
          }
          s_selected_media_path = "media/" + new_name;
          ImGui::CloseCurrentPopup();
        }
      }
      ImGui::SameLine();
      if (ImGui::Button(ICON_FA_TIMES " Cancel", ImVec2(80, 0)))
        ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }

    if (s_selected_scene_id != 0) {
      MovieConfig cfg = get_movie_config(g_project.db.get());
      if (ImGui::BeginChild("##timeline", ImVec2(0, timeline_h), true, ImGuiWindowFlags_NoScrollbar)) {
        ImGui::Text("Timeline: %g s  |  %.0f fps", cfg.duration_sec, cfg.frame_rate);
        ImGui::SameLine();
        ImGui::Text("Frame width:");
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_MINUS, ImVec2(24, 0)) && s_pixels_per_frame > 2)
          s_pixels_per_frame--;
        ImGui::SameLine();
        ImGui::Text("%d px", s_pixels_per_frame);
        ImGui::SameLine();
        if (ImGui::Button(ICON_FA_PLUS, ImVec2(24, 0)) && s_pixels_per_frame < 128)
          s_pixels_per_frame++;

        const float drop_area_h = ImGui::GetContentRegionAvail().y;
        const float label_row_h = 18.f;
        const float inner_decor = 2.f * ImGui::GetStyle().WindowBorderSize + ImGui::GetStyle().ScrollbarSize;
        const float track_h = std::max(24.f, drop_area_h - label_row_h - inner_decor);
        const int total_frames = static_cast<int>(cfg.duration_sec * cfg.frame_rate + 0.5);
        const float content_w = static_cast<float>(total_frames * s_pixels_per_frame);
        const float ppf = static_cast<float>(s_pixels_per_frame);

        static int s_dragging_layer_id = 0;
        static int s_resize_layer_id = 0;
        static bool s_resize_left = false;
        static int s_live_start = 0, s_live_span = 1;

        if (s_selected_layer_scene_id != s_selected_scene_id) {
          s_selected_layer_id = 0;
          s_selected_layer_scene_id = s_selected_scene_id;
        }
        if (ImGui::BeginChild("##timeline_track", ImVec2(0, drop_area_h), true, ImGuiWindowFlags_HorizontalScrollbar)) {
          ImDrawList* dl = ImGui::GetWindowDrawList();
          ImVec2 p0 = ImGui::GetCursorScreenPos();
          const double dur = (cfg.duration_sec > 0) ? cfg.duration_sec : 1.0;
          for (double t = 0; t <= cfg.duration_sec; t += 1.0) {
            float x = p0.x + static_cast<float>(t / dur) * content_w;
            char buf[16];
            snprintf(buf, sizeof(buf), "%.0fs", t);
            ImGui::SetCursorScreenPos(ImVec2(x - 6, p0.y));
            ImGui::Text("%s", buf);
          }
          ImVec2 p0_track = ImVec2(p0.x, p0.y + label_row_h);
          ImVec2 p1_track = ImVec2(p0.x + content_w, p0.y + label_row_h + track_h);
          ImGui::SetCursorScreenPos(ImVec2(p0.x, p0.y + label_row_h));
          ImGui::Dummy(ImVec2(content_w, track_h));
          auto frame_from_mouse = [&]() {
            float mouse_x = ImGui::GetMousePos().x;
            float scroll_x = ImGui::GetScrollX();
            float win_x = ImGui::GetWindowPos().x;
            float content_x = mouse_x - win_x + scroll_x;
            int f = static_cast<int>(content_x / ppf);
            return std::max(0, std::min(total_frames, f));
          };
          if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload* payload = ImGui::AcceptDragDropPayload("CHYA_MEDIA")) {
              const char* path = static_cast<const char*>(payload->Data);
              if (path && path[0] && total_frames > 0) {
                int frame_index = frame_from_mouse();
                if (frame_index >= total_frames) frame_index = total_frames - 1;
                add_layer_at_frame(g_project.db.get(), s_selected_scene_id, frame_index, path);
              }
            }
            ImGui::EndDragDropTarget();
          }
          dl->AddRectFilled(p0_track, p1_track, IM_COL32(50, 50, 55, 255));
          dl->AddRect(p0_track, p1_track, IM_COL32(80, 80, 85, 255));
          for (double t = 0; t <= cfg.duration_sec; t += 1.0) {
            float x = p0_track.x + static_cast<float>(t / dur) * content_w;
            dl->AddLine(ImVec2(x, p0_track.y), ImVec2(x, p1_track.y), IM_COL32(90, 90, 95, 255));
          }

          std::vector<LayerRow> layers = list_layers(g_project.db.get(), s_selected_scene_id);
          const float edge_hit_w = 6.f;

          if (!ImGui::IsAnyItemActive()) {
            if (s_selected_layer_id != 0 && ImGui::IsKeyPressed(ImGuiKey_C) && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper)) {
              for (const LayerRow& L : layers)
                if (L.id == s_selected_layer_id) {
                  s_clipboard_path = L.image_path;
                  s_clipboard_frame_span = L.frame_span;
                  break;
                }
            } else if (ImGui::IsKeyPressed(ImGuiKey_V) && (ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeySuper) && !s_clipboard_path.empty()) {
              int paste_at = 0;
              for (const LayerRow& L : layers)
                if (L.id == s_selected_layer_id) {
                  paste_at = L.start_frame + L.frame_span;
                  break;
                }
              if (paste_at < total_frames)
                add_layer_at_frame(g_project.db.get(), s_selected_scene_id, paste_at, s_clipboard_path, s_clipboard_frame_span);
            }
          }

          for (const LayerRow& layer : layers) {
            int draw_start = (s_dragging_layer_id == layer.id || s_resize_layer_id == layer.id) ? s_live_start : layer.start_frame;
            int draw_span = (s_resize_layer_id == layer.id) ? s_live_span : layer.frame_span;
            float x0 = p0_track.x + draw_start * ppf;
            float x1 = p0_track.x + (draw_start + draw_span) * ppf;
            if (x1 <= x0) x1 = x0 + ppf;
            ImVec2 b0(x0, p0_track.y);
            ImVec2 b1(x1, p1_track.y);

            int layer_id = layer.id;
            ImGui::PushID(layer_id);

            bool on_left_edge = (ImGui::IsMouseHoveringRect(ImVec2(b0.x, b0.y), ImVec2(b0.x + edge_hit_w, b1.y)) && !s_dragging_layer_id);
            bool on_right_edge = (ImGui::IsMouseHoveringRect(ImVec2(b1.x - edge_hit_w, b0.y), ImVec2(b1.x, b1.y)) && !s_dragging_layer_id);
            if (on_left_edge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);
            if (on_right_edge) ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeEW);

            if (s_resize_layer_id == layer_id) {
              int frame = frame_from_mouse();
              if (s_resize_left) {
                int end_frame = layer.start_frame + layer.frame_span;
                int new_start = std::max(0, std::min(frame, end_frame - 1));
                int new_span = end_frame - new_start;
                if (new_span >= 1) {
                  s_live_start = new_start;
                  s_live_span = new_span;
                  update_layer_start_frame(g_project.db.get(), layer_id, new_start);
                  update_layer_span(g_project.db.get(), layer_id, new_span);
                }
              } else {
                int new_span = std::max(1, frame - layer.start_frame);
                if (new_span <= total_frames - layer.start_frame) {
                  s_live_span = new_span;
                  update_layer_span(g_project.db.get(), layer_id, new_span);
                }
              }
              if (!ImGui::IsMouseDown(0))
                s_resize_layer_id = 0;
            } else if (ImGui::IsMouseClicked(0) && (on_left_edge || on_right_edge)) {
              s_resize_layer_id = layer_id;
              s_resize_left = on_left_edge;
              s_live_start = layer.start_frame;
              s_live_span = layer.frame_span;
            }

            if (s_dragging_layer_id == layer_id) {
              int frame = frame_from_mouse();
              int new_span = layer.frame_span;
              int new_start = std::max(0, std::min(frame, total_frames - new_span));
              s_live_start = new_start;
              update_layer_start_frame(g_project.db.get(), layer_id, new_start);
              if (!ImGui::IsMouseDown(0))
                s_dragging_layer_id = 0;
            } else {
              ImGui::SetCursorScreenPos(b0);
              ImGui::InvisibleButton("##clip", ImVec2(b1.x - b0.x, b1.y - b0.y));
              if (ImGui::IsItemClicked(0) && s_dragging_layer_id == 0 && s_resize_layer_id == 0) {
                s_selected_layer_id = layer_id;
                s_selected_layer_scene_id = s_selected_scene_id;
                s_selected_media_path.clear();
              }
              if (ImGui::IsItemHovered() && !on_left_edge && !on_right_edge)
                ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
              if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0) && s_dragging_layer_id == 0 && s_resize_layer_id == 0)
                s_dragging_layer_id = layer_id;
            }

            int thumb_w = 0, thumb_h = 0;
            ImTextureID clip_tex = get_thumbnail_texture(g_project.path, layer.image_path, &thumb_w, &thumb_h);
            if (clip_tex) {
              float clip_w = b1.x - b0.x, clip_h = b1.y - b0.y;
              float uv_left = 0.f, uv_right = 1.f, uv_top = 0.f, uv_bottom = 1.f;
              if (thumb_w > 0 && thumb_h > 0 && clip_w > 0 && clip_h > 0) {
                float img_aspect = (float)thumb_w / (float)thumb_h;
                float clip_aspect = clip_w / clip_h;
                if (img_aspect > clip_aspect) {
                  float crop = (1.f - clip_aspect / img_aspect) * 0.5f;
                  uv_left = crop;
                  uv_right = 1.f - crop;
                } else {
                  float crop = (1.f - img_aspect / clip_aspect) * 0.5f;
                  uv_top = crop;
                  uv_bottom = 1.f - crop;
                }
              }
              dl->AddImage(clip_tex, b0, b1, ImVec2(uv_left, uv_top), ImVec2(uv_right, uv_bottom), IM_COL32(255, 255, 255, 255));
              dl->AddRectFilled(b0, b1, IM_COL32(0, 0, 0, 140));
            } else {
              dl->AddRectFilled(b0, b1, IM_COL32(50, 60, 75, 255));
            }
            dl->AddRect(b0, b1, IM_COL32(90, 100, 120, 255));
            if (s_selected_layer_id == layer_id)
              dl->AddRect(b0, b1, IM_COL32(255, 255, 0, 255), 0.f, 0, 3.f);

            std::string label_name = fs::path(layer.image_path).filename().string();
            char buf[256];
            snprintf(buf, sizeof(buf), "%s  •  %d f", label_name.c_str(), draw_span);
            ImVec2 tsz = ImGui::CalcTextSize(buf);
            const float pad = 5.f;
            ImVec2 tpos(b0.x + pad, (b0.y + b1.y - tsz.y) * 0.5f);
            ImVec4 clip_rect(b0.x + pad, b0.y, b1.x - pad, b1.y);
            dl->AddText(ImGui::GetFont(), ImGui::GetFontSize(), tpos, IM_COL32(255, 255, 255, 255), buf, nullptr, 0.f, &clip_rect);

            ImGui::PopID();
          }

          if (s_resize_layer_id != 0 && !ImGui::IsMouseDown(0))
            s_resize_layer_id = 0;
          if (s_dragging_layer_id != 0 && !ImGui::IsMouseDown(0))
            s_dragging_layer_id = 0;
        }
        ImGui::EndChild();
      }
      ImGui::EndChild();
    }
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
}

void render_frame(GLFWwindow* window) {
  ImGui::Render();
  int w, h;
  glfwGetFramebufferSize(window, &w, &h);
  glViewport(0, 0, w, h);
  glClearColor(kClearColorR, kClearColorG, kClearColorB, kClearColorA);
  glClear(GL_COLOR_BUFFER_BIT);
  ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
  glfwSwapBuffers(window);
}

}  // namespace

int main() {
  if (!glfwInit())
    return 1;

  GLFWwindow* window = create_app_window();
  if (!window) {
    glfwTerminate();
    return 1;
  }
  g_main_window = window;

  init_imgui(window);
  glfwSetDropCallback(window, drop_callback);

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    if (g_play_window) {
      if (glfwWindowShouldClose(g_play_window)) {
        glfwDestroyWindow(g_play_window);
        g_play_window = nullptr;
      } else {
        MovieConfig cfg = get_movie_config(g_project.db.get());
        const int total_frames = get_scene_used_frame_count(g_project.db.get(), g_play_scene_id);
        double now = glfwGetTime();
        double elapsed = now - g_play_start_time;
        int frame = total_frames > 0 ? (static_cast<int>(elapsed * cfg.frame_rate) % total_frames) : 0;
        std::string path = get_image_at_frame(g_project.db.get(), g_play_scene_id, frame);
        GLuint tex = 0;
        if (!path.empty()) {
          ImTextureID tid = get_thumbnail_texture(g_project.path, path);
          if (tid) tex = (GLuint)(intptr_t)tid;
        }

        glfwMakeContextCurrent(g_play_window);
        int pw = 0, ph = 0;
        glfwGetFramebufferSize(g_play_window, &pw, &ph);
        glViewport(0, 0, pw, ph);
        glClearColor(0.1f, 0.1f, 0.12f, 1.f);
        glClear(GL_COLOR_BUFFER_BIT);
        if (tex != 0 && g_project.db) {
          if (g_quad_program == 0)
            g_quad_program = create_quad_program();
          if (g_quad_program != 0)
            draw_textured_quad(g_quad_program, tex);
        }
        glfwSwapBuffers(g_play_window);
        glfwMakeContextCurrent(window);
      }
    }

    begin_frame();
    draw_ui();
    render_frame(window);
  }

  if (g_play_window) {
    glfwDestroyWindow(g_play_window);
    g_play_window = nullptr;
  }
  shutdown_imgui();
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
