/*
 * Copyright 2000-2016 JetBrains s.r.o.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 * http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "fsnotifier.h"

#include <dirent.h>
#include <errno.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <syslog.h>
#include <unistd.h>


#define WATCH_COUNT_NAME "/proc/sys/fs/inotify/max_user_watches"

#define DEFAULT_SUBDIR_COUNT 5

typedef struct _watch_path {
  int path_len;
  char path[];
} watch_path;

typedef struct __watch_node {
  int wd;
  struct __watch_node* parent;
  array* kids;
  array* paths;
} watch_node;

static int inotify_fd = -1;
static int watch_count = 0;
static table* watches;
static bool limit_reached = false;
static void (* callback)(const char*, int) = NULL;

#define EVENT_SIZE (sizeof(struct inotify_event))
#define EVENT_BUF_LEN (2048 * (EVENT_SIZE + 16))
static char event_buf[EVENT_BUF_LEN];

static char path_buf[2 * PATH_MAX];

static void read_watch_descriptors_count();
static void watch_limit_reached();


bool init_inotify() {
  inotify_fd = inotify_init();
  if (inotify_fd < 0) {
    int e = errno;
    userlog(LOG_ERR, "inotify_init: %s", strerror(e));
    if (e == EMFILE) {
      message(MSG_INSTANCE_LIMIT);
    }
    return false;
  }
  userlog(LOG_DEBUG, "inotify fd: %d", get_inotify_fd());

  read_watch_descriptors_count();
  if (watch_count <= 0) {
    close(inotify_fd);
    inotify_fd = -1;
    return false;
  }
  userlog(LOG_INFO, "inotify watch descriptors: %d", watch_count);

  watches = table_create(watch_count);
  if (watches == NULL) {
    userlog(LOG_ERR, "out of memory");
    close(inotify_fd);
    inotify_fd = -1;
    return false;
  }

  return true;
}

static void read_watch_descriptors_count() {
  FILE* f = fopen(WATCH_COUNT_NAME, "r");
  if (f == NULL) {
    userlog(LOG_ERR, "can't open %s: %s", WATCH_COUNT_NAME, strerror(errno));
    return;
  }

  char* str = read_line(f);
  if (str == NULL) {
    userlog(LOG_ERR, "can't read from %s", WATCH_COUNT_NAME);
  }
  else {
    watch_count = atoi(str);
  }

  fclose(f);
}


void set_inotify_callback(void (* _callback)(const char*, int)) {
  callback = _callback;
}


int get_inotify_fd() {
  return inotify_fd;
}


#define EVENT_MASK IN_MODIFY | IN_ATTRIB | IN_CREATE | IN_DELETE | IN_MOVE | IN_DELETE_SELF | IN_MOVE_SELF

static int add_watch(int path_len, watch_node* parent) {
  int wd = inotify_add_watch(inotify_fd, path_buf, EVENT_MASK);
  if (wd < 0) {
    if (errno == EACCES || errno == ENOENT) {
      userlog(LOG_DEBUG, "inotify_add_watch(%s): %s", path_buf, strerror(errno));
      return ERR_IGNORE;
    }
    else if (errno == ENOSPC) {
      userlog(LOG_WARNING, "inotify_add_watch(%s): %s", path_buf, strerror(errno));
      watch_limit_reached();
      return ERR_CONTINUE;
    }
    else {
      userlog(LOG_ERR, "inotify_add_watch(%s): %s", path_buf, strerror(errno));
      return ERR_ABORT;
    }
  }
  else {
    userlog(LOG_DEBUG, "watching %s: %d", path_buf, wd);
  }

  watch_node* node = table_get(watches, wd);
  if (node != NULL) {
    if (node->wd != wd) {
      userlog(LOG_ERR, "table error: corruption at %d:%s / %d:%s)", wd, path_buf, node->wd, ((watch_path*)array_get(node->paths, 0))->path);
      return ERR_ABORT;
    }
    char buf2[PATH_MAX];
    const char* normalized_path_buff = realpath(path_buf, buf2);
    for (int i = 0; i < array_size(node->paths); i++) {
      const watch_path* wp = array_get(node->paths, i);
      if (strcmp(wp->path, path_buf) != 0) {
        char buf1[PATH_MAX] ;
        const char* normalized1 = realpath(wp->path, buf1);
        if (normalized1 == NULL || normalized_path_buff == NULL) {
          userlog(LOG_ERR, "table error: unable to determine real path of either directory %s, %s)", path_buf, ((watch_path*)array_get(node->paths, 0))->path);
          return ERR_ABORT;
        } else if (strcmp(normalized1, normalized_path_buff) == 0) {
          userlog(LOG_INFO, "intersection at %d: (new %s, existing %s, real %s)", wd, path_buf, ((watch_path*)array_get(node->paths, 0))->path, normalized1);
          return ERR_IGNORE;
        }
      }
    }

    //If an interection was not found, we have a hard link we need to add the path to
    watch_path* wp = malloc(sizeof(watch_path) + path_len + 1);
    memcpy(wp->path, path_buf, path_len + 1);
    wp->path_len = path_len;
    array_push(node->paths, wp);
    userlog(LOG_INFO, "shared inode at %d: (new %s, existing %s)", wd, path_buf, ((watch_path*)array_get(node->paths, 0))->path);

    return wd;
  }

  node = malloc(sizeof(watch_node));
  CHECK_NULL(node, ERR_ABORT);
  node->paths = array_create(1);
  watch_path* wp = malloc(sizeof(watch_path) + path_len + 1);
  memcpy(wp->path, path_buf, path_len + 1);
  wp->path_len = path_len;
  array_push(node->paths, wp);
  node->wd = wd;
  node->parent = parent;
  node->kids = NULL;

  if (parent != NULL) {
    if (parent->kids == NULL) {
      parent->kids = array_create(DEFAULT_SUBDIR_COUNT);
      CHECK_NULL(parent->kids, ERR_ABORT);
    }
    CHECK_NULL(array_push(parent->kids, node), ERR_ABORT);
  }

  if (table_put(watches, wd, node) == NULL) {
    userlog(LOG_ERR, "table error: unable to put (%d:%s)", wd, path_buf);
    return ERR_ABORT;
  }

  return wd;
}

static void watch_limit_reached() {
  if (!limit_reached) {
    limit_reached = true;
    message(MSG_WATCH_LIMIT);
  }
}

static void rm_watch(int wd, bool update_parent) {
  watch_node* node = table_get(watches, wd);
  if (node == NULL) {
    return;
  }

  userlog(LOG_DEBUG, "unwatching %s: %d (%p)", ((watch_path*)array_get(node->paths, 0))->path, node->wd, node);

  if (inotify_rm_watch(inotify_fd, node->wd) < 0) {
    userlog(LOG_DEBUG, "inotify_rm_watch(%d:%s): %s", node->wd, ((watch_path*)array_get(node->paths, 0))->path, strerror(errno));
  }

  for (int i=0; i<array_size(node->kids); i++) {
    watch_node* kid = array_get(node->kids, i);
    if (kid != NULL) {
      rm_watch(kid->wd, false);
    }
  }

  if (update_parent && node->parent != NULL) {
    for (int i=0; i<array_size(node->parent->kids); i++) {
      if (array_get(node->parent->kids, i) == node) {
        array_put(node->parent->kids, i, NULL);
        break;
      }
    }
  }

  array_delete(node->kids);
  free(node);
  table_put(watches, wd, NULL);
}


static int walk_tree(int path_len, watch_node* parent, bool recursive, array* mounts) {
  for (int j=0; j<array_size(mounts); j++) {
    char* mount = array_get(mounts, j);
    if (strncmp(path_buf, mount, strlen(mount)) == 0) {
      userlog(LOG_DEBUG, "watch path '%s' crossed mount point '%s' - skipping", path_buf, mount);
      return ERR_IGNORE;
    }
  }

  DIR* dir = NULL;
  if (recursive) {
    if ((dir = opendir(path_buf)) == NULL) {
      if (errno == EACCES || errno == ENOENT || errno == ENOTDIR) {
        userlog(LOG_DEBUG, "opendir(%s): %d", path_buf, errno);
        return ERR_IGNORE;
      }
      else {
        userlog(LOG_ERR, "opendir(%s): %s", path_buf, strerror(errno));
        return ERR_CONTINUE;
      }
    }
  }

  int id = add_watch(path_len, parent);

  if (dir == NULL) {
    return id;
  }
  else if (id < 0) {
    closedir(dir);
    return id;
  }

  path_buf[path_len] = '/';

  struct dirent* entry;
  while ((entry = readdir(dir)) != NULL) {
    if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
      continue;
    }
    if (entry->d_type != DT_UNKNOWN && entry->d_type != DT_DIR) {
      continue;
    }

    int name_len = strlen(entry->d_name);
    memcpy(path_buf + path_len + 1, entry->d_name, name_len + 1);

    if (entry->d_type == DT_UNKNOWN) {
      struct stat st;
      if (stat(path_buf, &st) != 0) {
        userlog(LOG_DEBUG, "(DT_UNKNOWN) stat(%s): %d", path_buf, errno);
        continue;
      }
      if (!S_ISDIR(st.st_mode)) {
        continue;
      }
    }

    int subdir_id = walk_tree(path_len + 1 + name_len, table_get(watches, id), recursive, mounts);
    if (subdir_id < 0 && subdir_id != ERR_IGNORE) {
      rm_watch(id, true);
      id = subdir_id;
      break;
    }
  }

  closedir(dir);
  return id;
}


int watch(const char* root, array* mounts) {
  bool recursive = true;
  if (root[0] == '|') {
    root++;
    recursive = false;
  }

  int path_len = strlen(root);
  if (root[path_len - 1] == '/') {
    --path_len;
  }

  struct stat st;
  if (stat(root, &st) != 0) {
    if (errno == ENOENT) {
      return ERR_MISSING;
    }
    else if (errno == EACCES || errno == ELOOP || errno == ENAMETOOLONG || errno == ENOTDIR) {
      userlog(LOG_INFO, "stat(%s): %s", root, strerror(errno));
      return ERR_CONTINUE;
    }
    else {
      userlog(LOG_ERR, "stat(%s): %s", root, strerror(errno));
      return ERR_ABORT;
    }
  }

  if (S_ISREG(st.st_mode)) {
    recursive = false;
  }
  else if (!S_ISDIR(st.st_mode)) {
    userlog(LOG_WARNING, "unexpected node type: %s, %d", root, st.st_mode);
    return ERR_IGNORE;
  }

  memcpy(path_buf, root, path_len);
  path_buf[path_len] = '\0';
  return walk_tree(path_len, NULL, recursive, mounts);
}


void unwatch(int id) {
  rm_watch(id, true);
}


static bool process_inotify_event(struct inotify_event* event) {
  watch_node* node = table_get(watches, event->wd);
  if (node == NULL) {
    return true;
  }

  bool is_dir = (event->mask & IN_ISDIR) == IN_ISDIR;
  userlog(LOG_DEBUG, "inotify: wd=%d mask=%d dir=%d name=%s", event->wd, event->mask & (~IN_ISDIR), is_dir, ((watch_path*)array_get(node->paths, 0))->path);

  for (int i = 0; i < array_size(node->paths); i++) {
    watch_path* wp = array_get(node->paths, i);
    int path_len = wp->path_len;
     memcpy(path_buf, wp->path, path_len + 1);
     if (event->len > 0) {
       path_buf[path_len] = '/';
       int name_len = strlen(event->name);
       memcpy(path_buf + path_len + 1, event->name, name_len + 1);
       path_len += name_len + 1;
     }

     if (callback != NULL) {
       (*callback)(path_buf, event->mask);
     }

     if (is_dir && event->mask & (IN_CREATE | IN_MOVED_TO)) {
       int result = walk_tree(path_len, node, true, NULL);
       if (result < 0 && result != ERR_IGNORE && result != ERR_CONTINUE) {
         return false;
       }
     }

     if (is_dir && event->mask & (IN_DELETE | IN_MOVED_FROM)) {
       for (int i=0; i<array_size(node->kids); i++) {
         watch_node* kid = array_get(node->kids, i);
         if (array_size(kid->paths) == 1) {
           watch_path* wp = array_get(kid->paths, 0);
           if (kid != NULL && strncmp(path_buf, wp->path, wp->path_len) == 0) {
             rm_watch(kid->wd, false);
             array_put(node->kids, i, NULL);
             break;
           }
         } else {
          //TODO
         }
       }
     }
   }

  return true;
}


bool process_inotify_input() {
  ssize_t len = read(inotify_fd, event_buf, EVENT_BUF_LEN);
  if (len < 0) {
    userlog(LOG_ERR, "read: %s", strerror(errno));
    return false;
  }

  int i = 0;
  while (i < len) {
    struct inotify_event* event = (struct inotify_event*) &event_buf[i];
    i += EVENT_SIZE + event->len;

    if (event->mask & IN_IGNORED) {
      continue;
    }
    if (event->mask & IN_Q_OVERFLOW) {
      userlog(LOG_INFO, "event queue overflow");
      continue;
    }

    if (!process_inotify_event(event)) {
      return false;
    }
  }

  return true;
}


void close_inotify() {
  if (watches != NULL) {
    table_delete(watches);
  }

  if (inotify_fd >= 0) {
    close(inotify_fd);
  }
}
