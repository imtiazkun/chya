#import <AppKit/AppKit.h>
#include "folder_picker.h"
#include <cstring>

bool pick_project_folder(char* out_path, std::size_t out_path_max) {
  if (!out_path || out_path_max == 0) return false;
  out_path[0] = '\0';
  NSOpenPanel* panel = [NSOpenPanel openPanel];
  panel.canChooseDirectories = YES;
  panel.canChooseFiles = NO;
  panel.canCreateDirectories = YES;
  panel.allowsMultipleSelection = NO;
  if ([panel runModal] != NSModalResponseOK) return false;
  NSURL* url = panel.URL;
  if (!url) return false;
  NSString* path = url.path;
  if (!path) return false;
  const char* cstr = [path UTF8String];
  if (!cstr) return false;
  std::size_t len = std::strlen(cstr);
  if (len >= out_path_max) return false;
  std::memcpy(out_path, cstr, len + 1);
  return true;
}

bool pick_save_file(char* out_path, std::size_t out_path_max, const char* default_name) {
  if (!out_path || out_path_max == 0) return false;
  out_path[0] = '\0';
  NSSavePanel* panel = [NSSavePanel savePanel];
  panel.canCreateDirectories = YES;
  if (default_name && default_name[0])
    panel.nameFieldStringValue = [NSString stringWithUTF8String:default_name];
  if ([panel runModal] != NSModalResponseOK) return false;
  NSURL* url = panel.URL;
  if (!url) return false;
  NSString* path = url.path;
  if (!path) return false;
  const char* cstr = [path UTF8String];
  if (!cstr) return false;
  std::size_t len = std::strlen(cstr);
  if (len >= out_path_max) return false;
  std::memcpy(out_path, cstr, len + 1);
  return true;
}
