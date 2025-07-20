#include "vcr.h"


void print_backtrace(void) {
#if defined(__linux__) || defined(__GLIBC__)

  void *stack_frames[64];
  int frame_count;
  char **frame_strings;

  // Get the stack frames
  frame_count = backtrace(stack_frames, 64);

  // Convert addresses to strings
  frame_strings = backtrace_symbols(stack_frames, frame_count);
  if (frame_strings == NULL) {
    perror("backtrace_symbols");
    exit(EXIT_FAILURE);
  }

  // Print the backtrace
  fprintf(stderr, "\nBacktrace:\n");
  for (int i = 0; i < frame_count; i++) {
    fprintf(stderr, "  [%d] %s\n", i, frame_strings[i]);
  }

  free(frame_strings);
#else
  printf("cannot print a backtrace on non linux systems\n");
#endif
}

void log_msg(log_level level, const char* file, const char* func, int line, const char* fmt, ...) {

  static const char* level_strings[] = {
    "INFO",
    "WARN",
    "ERROR",
    "FATAL"
};

  time_t now;
  time(&now);
  char* date = ctime(&now);
  date[strlen(date) - 1] = '\0'; // Remove newline

  fprintf(stderr, "%s [%s] %s:%s:%d: ", date, level_strings[level], file, func, line);

  va_list args;
  va_start(args, fmt);
  vfprintf(stderr, fmt, args);
  va_end(args);

  fprintf(stderr, "\n");
  fflush(stderr);
}

void print_assert_details(const char* expr, const char* file, int line, const char* func) {
  fprintf(stderr, "\nAssertion failed!\n");
  fprintf(stderr, "Expression: %s\n", expr);
  fprintf(stderr, "Location  : %s:%d\n", file, line);
  fprintf(stderr, "Function  : %s\n", func);
}

void assert_fail_with_backtrace(const char* expr, const char* file, int line, const char* func) {
  print_assert_details(expr, file, line, func);
  print_backtrace();
  abort();
}

bool path_exists(const char *path) {
  return access(path, F_OK) == 0 ? true : false;
}


// Helper function to read file contents
char* read_file(const char* filepath) {
  FILE* file = fopen(filepath, "rb");
  if (!file) {
    return NULL;
  }

  fseek(file, 0, SEEK_END);
  long size = ftell(file);
  fseek(file, 0, SEEK_SET);

  char* content = (char*)malloc(size + 1);
  if (!content) {
    fclose(file);
    return NULL;
  }

  size_t read_size = fread(content, 1, size, file);
  content[read_size] = '\0';

  fclose(file);
  return content;
}