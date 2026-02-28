#pragma once
#include <cstddef>

// Opens native folder picker. Returns true and writes selected path into out_path (null-terminated) if user picked a folder.
// out_path_max is the size of the buffer. Returns false if cancelled or unavailable.
bool pick_project_folder(char* out_path, std::size_t out_path_max);

// Opens native save file picker. Returns true and writes selected path into out_path if user picked a path.
// default_name can be e.g. "output.mp4". Returns false if cancelled or unavailable.
bool pick_save_file(char* out_path, std::size_t out_path_max, const char* default_name);
