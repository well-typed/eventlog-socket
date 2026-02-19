#include "./string.h"

HIDDEN char *ess_strdup(const char *const str) {
  if (str == NULL) {
    errno = EINVAL;
    return NULL;
  }
  char *str_dup = malloc(strlen(str) + 1);
  if (str_dup == NULL) {
    return NULL; // `malloc` sets errno.
  }
  strcpy(str_dup, str);
  return str_dup;
}

HIDDEN char *ess_strndup(const size_t str_len, const char str[str_len + 1]) {
  if (str == NULL) {
    errno = EINVAL;
    return NULL;
  }
  char *str_dup = malloc(str_len + 1);
  if (str_dup == NULL) {
    return NULL; // `malloc` sets errno.
  }
  strncpy(str_dup, str, str_len);
  str_dup[str_len] = '\0';
  return str_dup;
}
