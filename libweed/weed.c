/* WEED is free software; you can redistribute it and/or
   modify it under the terms of the GNU Lesser General Public
   License as published by the Free Software Foundation; either
   version 3 of the License, or (at your option) any later version.

   Weed is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
   Lesser General Public License for more details.

   You should have received a copy of the GNU Lesser General Public
   License along with this source code; if not, write to the Free Software
   Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA

   Weed is developed by:
   Gabriel "Salsaman" Finch - http://lives-video.com

   partly based on LiViDO, which is developed by:
   Niels Elburg - http://veejay.sf.net
   Denis "Jaromil" Rojo - http://freej.dyne.org
   Tom Schouten - http://zwizwa.fartit.com
   Andraz Tori - http://cvs.cinelerra.org

   reviewed with suggestions and contributions from:
   Silvano "Kysucix" Galliani - http://freej.dyne.org
   Kentaro Fukuchi - http://megaui.net/fukuchi
   Jun Iio - http://www.malib.net
   Carlo Prelz - http://www2.fluido.as:8080/
*/

/* (C) G. Finch, 2005 - 2020 */

// implementation of libweed with or without glib's slice allocator, with or without thread safety (needs pthread)

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#ifdef USE_GSLICE
#include <glib.h>
#endif

#define __LIBWEED__

#ifndef NEED_LOCAL_WEED
#include <weed/weed.h>
#else
#include "weed.h"
#endif

#define WEED_MAGIC_HASH 0x7C9EBD07  // the magic number
#define WEED_FLAG_OP_DELETE WEED_FLAG_RESERVED_0

#if defined __GNUC__ && !defined WEED_IGN_GNUC_OPT
#define GNU_FLATTEN  __attribute__((flatten)) // inline all function calls
#define GNU_CONST  __attribute__((const))
#define GNU_HOT  __attribute__((hot))
#define GNU_PURE  __attribute__((pure))
#else
#define GNU_FLATTEN
#define GNU_CONST
#define GNU_HOT
#define GNU_PURE
#endif

// Define EXPORTED for any platform
#if defined _WIN32 || defined __CYGWIN__ || defined IS_MINGW
#ifdef WIN_EXPORT
#ifdef __GNUC__
#define EXPORTED __attribute__ ((dllexport))
#else
#define EXPORTED __declspec(dllexport) // Note: actually gcc seems to also supports this syntax.
#endif
#else
#ifdef __GNUC__
#define EXPORTED __attribute__ ((dllimport))
#else
#define EXPORTED __declspec(dllimport) // Note: actually gcc seems to also supports this syntax.
#endif
#endif
#define NOT_EXPORTED
#else
#if __GNUC__ >= 4
#define EXPORTED __attribute__ ((visibility ("default")))
#define NOT_EXPORTED  __attribute__ ((visibility ("hidden")))
#else
#define EXPORTED
#define NOT_EXPORTED
#endif
#endif

#ifdef _BUILD_THREADSAFE_
#include <pthread.h>

typedef struct {
  pthread_rwlock_t count_lock;
  pthread_mutex_t trr_mutex;
  pthread_mutex_t trw_mutex;
} pthread6;

typedef struct {
  pthread_rwlock_t travel_lock;
  pthread_rwlock_t rwlock;
  pthread_mutex_t mutex;
} pthread3;

typedef struct {
  pthread3 pth3;
} leaf_priv_data_t;

typedef struct {
  pthread3 pth3;
  pthread6 pth6;
} plant_priv_data_t;


#define rw_unlock(obj) do {			    \
    if ((obj)) pthread_rwlock_unlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->rwlock);} while (0)
#define rw_writelock(obj) do { \
    if ((obj)) pthread_rwlock_wrlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->rwlock);} while (0)
#define rw_readlock(obj) do { \
    if ((obj)) pthread_rwlock_rdlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->rwlock);} while (0)
#define rwt_unlock(obj) do {			    \
    if ((obj)) pthread_rwlock_unlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->travel_lock);} while (0)
#define rwt_writelock(obj) do { \
    if ((obj)) pthread_rwlock_wrlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->travel_lock);} while (0)
#define rwt_readlock(obj) do { \
    if ((obj)) pthread_rwlock_rdlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->travel_lock);} while (0)
#define rwt_try_readlock(obj) \
  ((obj) ? pthread_rwlock_tryrdlock(&((pthread6 *) \
				      (((weed_leaf_t *)(obj))->private_data))->travel_lock) : 0)
#define rwt_try_writelock(obj) \
  ((obj) ? pthread_rwlock_trywrlock(&((pthread6 *)			\
				      (((weed_leaf_t *)(obj))->private_data))->travel_lock) : 0)


#define rwt_writer_mutex_trylock(obj) pthread_mutex_trylock(&((pthread6 *)	\
							      (((weed_leaf_t *)(obj))->private_data))->trw_mutex)
#define rwt_writer_mutex_lock(obj) do { \
    if ((obj)) pthread_mutex_lock(&((pthread6 *)			\
				    (((weed_leaf_t *)(obj))->private_data))->trw_mutex);} while (0)
#define rwt_writer_mutex_unlock(obj) do {					\
    if ((obj)) pthread_mutex_unlock(&((pthread6 *)			\
				      (((weed_leaf_t *)(obj))->private_data))->trw_mutex);} while (0)

#define rwt_reader_mutex_trylock(obj) pthread_mutex_trylock(&((pthread6 *)	\
							      (((weed_leaf_t *)(obj))->private_data))->trr_mutex)
#define rwt_reader_mutex_lock(obj) do { \
    if ((obj)) pthread_mutex_lock(&((pthread6 *)			\
				    (((weed_leaf_t *)(obj))->private_data))->trr_mutex);} while (0)
#define rwt_reader_mutex_unlock(obj) do { \
    if ((obj)) pthread_mutex_unlock(&((pthread6 *)			\
				      (((weed_leaf_t *)(obj))->private_data))->trr_mutex);} while (0)

#define rwt_count_add(obj) do { \
    if ((obj)) pthread_rwlock_rdlock(&((pthread6 *) \
				       (((weed_leaf_t *)(obj))->private_data))->count_lock);} while (0)
#define rwt_count_sub(obj) do { \
    if ((obj)) pthread_rwlock_unlock(&((pthread6 *)			\
				       (((weed_leaf_t *)(obj))->private_data))->count_lock);} while (0)
#define rwt_count_wait(obj) do { \
    if ((obj)) pthread_rwlock_wrlock(&((pthread6 *)			\
				       (((weed_leaf_t *)(obj))->private_data))->count_lock); \
    rwt_count_sub((obj));} while (0)

#define return_unlock(obj, val) do {					\
    typeof(val) myval = (val); rw_unlock((obj)); return myval;} while (0)

static int rw_upgrade(weed_leaf_t *leaf, int block) {
  if (leaf) {
    pthread6 *pthgroup = (pthread6 *)leaf->private_data;
    // try to grab the mutex
    int ret = pthread_mutex_trylock(&pthgroup->mutex);
    if (ret) {
      // if we fail and !block, return with readlock held
      if (!block) return ret;
      else {
	// otherwise, drop the readlock in case a writer is blocked
	// the block until we do get mutex
	rw_unlock(leaf);
	pthread_mutex_lock(&pthgroup->mutex);
      }
    }
    else rw_unlock(leaf);
    // now we can wait for the writelock and then unlock mutex
    // subsequent writers will drop their readlocks and block on the mutex
    rw_writelock(leaf);
    pthread_mutex_unlock(&pthgroup->mutex);
  }
  return 0;
}

static int rwt_upgrade(weed_leaf_t *leaf, int have_rdlock, int is_del) {
  // grab the mutex, release the readlock held, grab a write lock, release the mutex,
  // release the writelock
  // if blocking is 0, then we return if we cannot get the mutex
  // return 0 if we got the write lock, otherwise
  if (leaf) {
    // try to lock the leaf
    if (rwt_writer_mutex_trylock(leaf)) {
      // otherwise, another thread is waitng for writelock
      // so we have to unlock to avoid a deadlock
      // (however, no thread trying to write will have this)
      if (have_rdlock) rwt_unlock(leaf);
      // then wait until we can grab the lock
      rwt_writer_mutex_lock(leaf);
    }
    else if (have_rdlock) rwt_unlock(leaf);
    // now we have the mutex lock, we grab the writelock
    // with the mutex held, other threads will be blocked
    // and released their readlocks, allowing us to proceed

    // we want to make getting writelock and setting OP_DELETE atomic
    rwt_reader_mutex_lock(leaf);
    rwt_writelock(leaf);
    // if it is a SET, then we release the mutex; the next writer
    // will block on writelock; subsequent writeers will block on the mutex
    if (is_del) leaf->flags |= WEED_FLAG_OP_DELETE;
    else rwt_writer_mutex_unlock(leaf);
    rwt_reader_mutex_unlock(leaf);
  }
  return 0;
}

#else
#define rw_unlock(obj)
#define rw_readlock(obj)
#define rwt_writer_mutex_trylock(obj)
#define rwt_writer_mutex_lock(obj)
#define rwt_writer_mutex_unlock(obj)
#define rwt_reader_mutex_trylock(obj)
#define rwt_reader_mutex_lock(obj)
#define rwt_reader_mutex_unlock(obj)
#define rwt_unlock(obj)
#define rwt_readlock(obj)
#define rwt_try_readlock(obj) 0
#define return_unlock(obj, val) return ((val))

static int rw_upgrade(weed_leaf_t *leaf, int block) {return 0;}
static int rwt_upgrade(weed_leaf_t *leaf, int have_rdlock, int is_del) {return 0;}
#endif

static int allbugfixes = 0;
static int debugmode = 0;

static int32_t _abi_ = WEED_ABI_VERSION;

EXPORTED int32_t weed_get_abi_version(void);
EXPORTED weed_error_t weed_init(int32_t abi, uint64_t init_flags);

static weed_plant_t *_weed_plant_new(int32_t plant_type) GNU_FLATTEN;
static weed_error_t _weed_plant_free(weed_plant_t *) GNU_FLATTEN;
static char **_weed_plant_list_leaves(weed_plant_t *, weed_size_t *nleaves) GNU_FLATTEN;
static weed_error_t _weed_leaf_get(weed_plant_t *, const char *key, int32_t idx,
				   weed_voidptr_t value) GNU_HOT;
static weed_error_t _weed_leaf_set(weed_plant_t *, const char *key, uint32_t seed_type,
				   weed_size_t num_elems, weed_voidptr_t values) GNU_FLATTEN;
static weed_size_t _weed_leaf_num_elements(weed_plant_t *, const char *key) GNU_FLATTEN;
static weed_size_t _weed_leaf_element_size(weed_plant_t *, const char *key, int32_t idx) GNU_FLATTEN;
static uint32_t _weed_leaf_seed_type(weed_plant_t *, const char *key) GNU_FLATTEN;
static uint32_t _weed_leaf_get_flags(weed_plant_t *, const char *key) GNU_FLATTEN;
static weed_error_t _weed_leaf_delete(weed_plant_t *, const char *key) GNU_FLATTEN;

/* host only functions */
static weed_error_t _weed_leaf_set_flags(weed_plant_t *, const char *key, uint32_t flags) GNU_FLATTEN;

static weed_error_t _weed_leaf_set_private_data(weed_plant_t *, const char *key, void *data)
  GNU_FLATTEN;
static weed_error_t _weed_leaf_get_private_data(weed_plant_t *, const char *key, void **data_return)
  GNU_FLATTEN;

/* internal functions */
static weed_leaf_t *weed_find_leaf(weed_plant_t *, const char *key, uint32_t *hash_ret)
  GNU_FLATTEN GNU_HOT;
static weed_leaf_t *weed_leaf_new(const char *key, uint32_t seed_type, uint32_t hash) GNU_FLATTEN;

static int weed_strcmp(const char *, const char *) GNU_HOT;
static weed_size_t weed_strlen(const char *) GNU_PURE;
static uint32_t weed_hash(const char *) GNU_PURE;

#ifdef USE_GSLICE
#define weed_malloc g_slice_alloc
#define weed_malloc_sizeof g_slice_new
#define weed_unmalloc_sizeof g_slice_free
#define weed_unmalloc_and_copy g_slice_free1

#if GLIB_CHECK_VERSION(2, 14, 0)
#define weed_malloc_and_copy(bsize, block) g_slice_copy(bsize, block)
#endif

#else
#define weed_malloc malloc
#define weed_malloc_sizeof(t) malloc(sizeof(t))
#define weed_unmalloc_sizeof(t, ptr) free(ptr)
#define weed_unmalloc_and_copy(size, ptr) free(ptr)
#endif

#ifndef weed_malloc_and_copy
#define weed_malloc_and_copy(size, src) memcpy(malloc(size), src, size)
#endif

#define weed_strdup(oldstring, size) (!oldstring ? (char *)NULL : \
				      size < _WEED_PADBYTES_ ? memcpy(leaf->padding, key, size + 1) : \
				      (char *)(weed_malloc_and_copy(weed_strlen(oldstring) + 1, \
								    oldstring)))

#define IS_VALID_SEED_TYPE(seed_type) ((seed_type >=64 || (seed_type >= 1 && seed_type <= 5) ? 1 : 0))

EXPORTED int32_t weed_get_abi_version(void) {return _abi_;}

EXPORTED weed_error_t weed_init(int32_t abi, uint64_t init_flags) {
  // this is called by the host in order for it to set its version of the functions

  // *the plugin should never call this, instead the plugin functions are passed to the plugin
  // from the host in the "host_info" plant*

  if (abi < 0 || abi > WEED_ABI_VERSION) return WEED_ERROR_BADVERSION;
  _abi_ = abi;

  if (init_flags & WEED_INIT_ALLBUGFIXES) allbugfixes = 1;
  if (init_flags & WEED_INIT_DEBUGMODE) debugmode = 1;

  if (debugmode) {
    fprintf(stderr, "Weed padding size is %d\n", _WEED_PADBYTES_);
  }

  weed_leaf_get = _weed_leaf_get;
  weed_leaf_delete = _weed_leaf_delete;
  weed_plant_free = _weed_plant_free;
  weed_plant_new = _weed_plant_new;
  weed_leaf_set = _weed_leaf_set;
  weed_plant_list_leaves = _weed_plant_list_leaves;
  weed_leaf_num_elements = _weed_leaf_num_elements;
  weed_leaf_element_size = _weed_leaf_element_size;
  weed_leaf_seed_type = _weed_leaf_seed_type;
  weed_leaf_get_flags = _weed_leaf_get_flags;
  weed_leaf_set_flags = _weed_leaf_set_flags;
  if (_abi_ >= 200) {
    weed_leaf_get_private_data = _weed_leaf_get_private_data;
    weed_leaf_set_private_data = _weed_leaf_set_private_data;
  }
  return WEED_SUCCESS;
}

#define weed_strlen(s) ((s) ? strlen((s)) : 0)
#define weed_strcmp(s1, s2) ((!(s1) || !(s2)) ? (s1 != s2) : strcmp(s1, s2))

#define HASHROOT 5381
static inline uint32_t weed_hash(const char *string) {
  for (uint32_t hash = HASHROOT;; hash += (hash << 5) + *(string++))
    if (!(*string)) return hash;}

#define weed_seed_is_ptr(seed_type) (seed_type >= 64 ? 1 : 0)

#define weed_seed_get_size(seed_type, size) (seed_type == WEED_SEED_FUNCPTR ? WEED_FUNCPTR_SIZE : \
					     weed_seed_is_ptr(seed_type) ? WEED_VOIDPTR_SIZE : \
					     (seed_type == WEED_SEED_BOOLEAN || \
					      seed_type == WEED_SEED_INT) ? 4 : \
					     seed_type == WEED_SEED_DOUBLE ? 8 : \
					     seed_type == WEED_SEED_INT64 ? 8 : \
					     seed_type == WEED_SEED_STRING ? size : 0)

static inline void *weed_data_free(weed_data_t **data, weed_size_t num_valid_elems,
				   weed_size_t num_elems, uint32_t seed_type) {
  int is_nonptr = !weed_seed_is_ptr(seed_type);
  for (weed_size_t i = 0; i < num_valid_elems; i++) {
    if (is_nonptr && data[i]->value.voidptr)
      weed_unmalloc_and_copy(data[i]->size, data[i]->value.voidptr);
    weed_unmalloc_sizeof(weed_data_t, data[i]);
  }
  weed_unmalloc_and_copy(num_elems * sizeof(weed_data_t *), data);
  return NULL;
}

static inline weed_data_t **weed_data_new(uint32_t seed_type, weed_size_t num_elems,
					  weed_voidptr_t values) {
  weed_data_t **data;
  if (!num_elems) return NULL;
  if (!(data = (weed_data_t **)weed_malloc(num_elems * sizeof(weed_data_t *)))) return NULL;
  else {
    char **valuec = (char **)values;
    weed_voidptr_t *valuep = (weed_voidptr_t *)values;
    weed_funcptr_t *valuef = (weed_funcptr_t *)values;
    int is_ptr = (weed_seed_is_ptr(seed_type));
    for (int i = 0; i < num_elems; i++) {
      if (!(data[i] = weed_malloc_sizeof(weed_data_t)))
	return weed_data_free(data, --i, num_elems, seed_type);
      if (seed_type == WEED_SEED_STRING) {
        data[i]->value.voidptr = (weed_voidptr_t)(((data[i]->size = weed_strlen(valuec[i])) > 0) ?
                                 (weed_voidptr_t)weed_malloc_and_copy(data[i]->size, valuec[i]) : NULL);
      } else {
        data[i]->size = weed_seed_get_size(seed_type, 0);
        if (seed_type == WEED_SEED_FUNCPTR)
	  memcpy(&data[i]->value.funcptr, &valuef[i], WEED_FUNCPTR_SIZE);
        else {
          if (is_ptr) memcpy(&data[i]->value.voidptr, &valuep[i], WEED_VOIDPTR_SIZE);
          else data[i]->value.voidptr =
		 (weed_voidptr_t)(weed_malloc_and_copy(data[i]->size,
						       (char *)values + i * data[i]->size));
        }}
      if (!is_ptr && !data[i]->value.voidptr && data[i]->size > 0) //ymemory error
        return weed_data_free(data, --i, num_elems, seed_type);
    }}
  return data;
}

static inline weed_leaf_t *weed_find_leaf(weed_plant_t *plant, const char *key, uint32_t *hash_ret) {
  uint32_t hash = WEED_MAGIC_HASH;
  weed_leaf_t *leaf = plant, *rwtleaf = NULL;
  int checkmode = 0;

  if (key && *key) {
    // if hash_ret is set then this is a setter looking for leaf
    // in this case it already has a rwt_writelock and does not need to check further
    if (!hash_ret) {
      /// grab rwt mutex
      /// if we get a readlock, then remove it at end
      /// othewise check flagbits, if op. is !SET, run in checking mode
      rwt_reader_mutex_lock(plant);
      if (rwt_try_readlock(plant)) {
	// another thread has writelock
	if (plant->flags & WEED_FLAG_OP_DELETE) checkmode = 1;
      }
      else rwt_unlock(plant);
      // this counts the number of readers running in non-check mode
      if (!checkmode) rwt_count_add(plant);
      rwt_reader_mutex_unlock(plant);
    }

    hash = weed_hash(key);

    while (leaf && (hash != leaf->key_hash || weed_strcmp((char *)leaf->key, (char *)key))) {
      leaf = leaf->next;
      if (checkmode && leaf) {
	// lock leaf so it cannot be freed till we have passed over it
	// also we will block if the next leaf is about to be adjusted
	rwt_readlock(leaf);
	rwt_unlock(rwtleaf); // does nothing if rwtleaf is NULL
	rwtleaf = leaf;
      }
    }
    if (leaf) {
      rw_readlock(leaf);
    }
    if (!hash_ret) {
      rwt_unlock(rwtleaf);
      if (!checkmode) {
	rwt_reader_mutex_lock(plant);
	rwt_count_sub(plant);
	rwt_reader_mutex_unlock(plant);
      }
    }
  }
  else rw_readlock(leaf);
  if (hash_ret) *hash_ret = hash;
  return leaf;
}

static inline void *weed_leaf_free(weed_leaf_t *leaf) {
#ifdef _BUILD_THREADSAFE_
  pthread6 *pthgroup = (pthread6 *)leaf->private_data;
#endif
  if (leaf->data)
    weed_data_free((void *)leaf->data, leaf->num_elements, leaf->num_elements, leaf->seed_type);
  if (leaf->key != leaf->padding) weed_unmalloc_and_copy(weed_strlen(leaf->key) + 1,
							 (void *)leaf->key);
#ifdef _BUILD_THREADSAFE_
  rw_unlock(leaf);
  rw_readlock(leaf);
  rw_upgrade(leaf, 1);
  rw_unlock(leaf);
  weed_unmalloc_sizeof(pthread6, pthgroup);
#endif
  weed_unmalloc_sizeof(weed_leaf_t, leaf);
  return NULL;
}

static inline weed_leaf_t *weed_leaf_new(const char *key, uint32_t seed_type, uint32_t hash) {
  weed_leaf_t *leaf;
#ifdef _BUILD_THREADSAFE_
  pthread6 *pthgroup;
#endif
  leaf = weed_malloc_sizeof(weed_leaf_t);
  if (!leaf) return NULL;
  leaf->key_hash = hash;
  leaf->next = NULL;
  leaf->key = weed_strdup(key, weed_strlen(key));
  if (!leaf->key)
    {weed_unmalloc_sizeof(weed_leaf_t, leaf); return NULL;}
  leaf->num_elements = 0;
  leaf->seed_type = seed_type;
  leaf->flags = 0;
  leaf->data = NULL;
#ifdef _BUILD_THREADSAFE_
  pthgroup = weed_malloc_sizeof(pthread6);
  if (!pthgroup)
    {weed_unmalloc_sizeof(weed_leaf_t, leaf);
      if (leaf->key != leaf->padding)
	weed_unmalloc_and_copy(weed_strlen(leaf->key + 1) + 2,
			       (void *)leaf->key);
      return NULL;}
  pthread_mutex_init(&pthgroup->mutex, NULL);
  pthread_mutex_init(&pthgroup->trr_mutex, NULL);
  pthread_mutex_init(&pthgroup->trw_mutex, NULL);
  pthread_rwlock_init(&pthgroup->rwlock, NULL);
  pthread_rwlock_init(&pthgroup->travel_lock, NULL);
  pthread_rwlock_init(&pthgroup->count_lock, NULL);
  leaf->private_data = (void *)pthgroup;
#else
  leaf->private_data = NULL;
#endif
  return leaf;
}

static inline weed_error_t weed_leaf_append(weed_plant_t *plant, weed_leaf_t *newleaf) {
  // get transport lock, in case some other thread is waiting to delete
  // plant->next
  newleaf->next = plant->next;
  plant->next = newleaf;
  return WEED_SUCCESS;
}

static weed_error_t _weed_plant_free(weed_plant_t *plant) {
  weed_leaf_t *leaf, *leafprev = plant, *leafnext = plant->next;
  if (!plant) return WEED_SUCCESS;

  if (plant->flags & WEED_FLAG_UNDELETABLE) return WEED_ERROR_UNDELETABLE;

  // see: weed_leaf_delete
  rwt_upgrade(plant, 0, 1);
  rwt_count_wait(plant);

  /// hold on to mutex until we are done
  //rwt_mutex_unlock(plant);

  while ((leaf = leafnext)) {
    leafnext = leaf->next;
    if (leaf->flags & WEED_FLAG_UNDELETABLE) {
      leafprev = leaf;
    }
    else {
      leafprev->next = leafnext;
      rw_readlock(leaf);
      weed_leaf_free(leaf);
    }}

  if (!plant->next) {
    // remove lock temporarily just in case other threads were trying to grab a read lock
    rwt_unlock(plant);
    rwt_writer_mutex_unlock(plant);

    rwt_upgrade(plant, 0, 1);
    rwt_count_wait(plant);
    rwt_unlock(plant);
    rwt_writer_mutex_unlock(plant);

    rw_readlock(plant);
    rw_upgrade(plant, 1);
    weed_leaf_free(plant);
    return WEED_SUCCESS;
  }
  plant->flags ^= WEED_FLAG_OP_DELETE;
  rwt_unlock(plant);
  rwt_writer_mutex_unlock(plant);
  return WEED_ERROR_UNDELETABLE;
}

static weed_error_t _weed_leaf_delete(weed_plant_t *plant, const char *key) {
  weed_leaf_t *leaf, *leafprev = NULL;
  uint32_t hash = weed_hash(key);

  // lock out all other deleters, setters
  // we will grab the mutex, forcing other rivals to drop their readlocks
  // (which actually none of them will have in the present implementation)
  // and then block.
  // and temporarily stop the flow of new readers
  // then we get writelock, and release the mutex. This allows new readers in
  // prior to this, we set
  // WEED_FLAG_OP_DELETE, forcing new readers to run in checkmode, locking
  // and unlocking the the travel rwlock as they go
  // readers which are in running in non-check mode will be counted in count lock
  // so before doing any updates we will first wait for count to reach zero


  // Before deleting the target, we will witelock the prev node travel mutex.
  // Since readers are now in checkmode, they will be blocked there.
  // once we get that writelock, we know that there can be no readers on prev node
  // we can repoint the prev node and unlock the prev node travel writelock
  // knowing that further readers will now be routed past the leaf to be deleted
  // We can also unlock the plant, since new readers no longer need to run in check mode.
  // finally, before deleting leaf, we must ensure there are no reamaining readers on it
  // we do this by first obtaining a travel rwlock on it, then obtaining a normal
  // writelock
  // we then know it is safe to delete leaf !

  rwt_upgrade(plant, 0, 1);

  leaf = plant;

  while (leaf && (leaf->key_hash != hash || weed_strcmp((char *)leaf->key, (char *)key))) {
    // no match

    // still have rwtlock on leafprev
    // still have rwtlock on leaf
    // we will grab rwtlock on next leaf
    if (leaf != plant) {
      rwt_unlock(leafprev); // does nothing if leafprev is NULL
      leafprev = leaf; // leafprev is still locked
    }
    leaf = leaf->next;
    rwt_readlock(leaf); // does nothing if leaf is NULL
  }
  // finish with rwtlock on prevprev. prev amd leaf
  if (!leaf || leaf == plant) {
    rwt_unlock(plant);
    if (leafprev != plant) rwt_unlock(leafprev);
    rwt_writer_mutex_unlock(plant);
    return WEED_ERROR_NOSUCH_LEAF;
  }

  if (leaf->flags & WEED_FLAG_UNDELETABLE) {
    rwt_unlock(plant);
    if (leafprev != leaf && leafprev != plant) rwt_unlock(leafprev);
    if (leaf != plant) rwt_unlock(leaf);
    rwt_writer_mutex_unlock(plant);
    return WEED_ERROR_UNDELETABLE;
  }

  // we dont update link until we have write trans. lock on leafprev
  // - after that, new readers will only get the new value, so we can be sure that
  // if we get trans write lock and data write lock on leaf then it is safe to free it
  // there can still be read / write locks on leafprev but we dont care about that

  // first, wait for non-checking readers to complete
  rwt_count_wait(plant);

  // block readers at leafprev
  if (leafprev != plant) rwt_upgrade(leafprev, 1, 0);

  // adjust the link
  leafprev->next = leaf->next;

  // and that is it, job done. Now we can free leaf at leisure
  plant->flags ^= WEED_FLAG_OP_DELETE;
  rwt_unlock(plant);
  if (leafprev != leaf && leafprev != plant) rwt_unlock(leafprev);
  rwt_writer_mutex_unlock(plant);

  // get a trans write link on leaf, once we have this, all readers have moved to the next
  // leaf, and we are almosy done
  rwt_upgrade(leaf, 1, 0);
  rwt_unlock(leaf);

  // wait for all readers / writers on leaf to complete
  rw_writelock(leaf);
  weed_leaf_free(leaf);

  return WEED_SUCCESS;
}

static weed_error_t _weed_leaf_set_flags(weed_plant_t *plant, const char *key, uint32_t flags) {
  weed_leaf_t *leaf;
  // strip any reserved bits from flags
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return WEED_ERROR_NOSUCH_LEAF;
  // block here, flags are important
  rw_upgrade(leaf, 1);
  leaf->flags = (leaf->flags & WEED_FLAGBITS_RESERVED) | (flags & ~WEED_FLAGBITS_RESERVED);
  return_unlock(leaf, WEED_SUCCESS);
}

static weed_error_t _weed_leaf_set_private_data(weed_plant_t *plant, const char *key, void *data) {
  weed_leaf_t *leaf;
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return WEED_ERROR_NOSUCH_LEAF;
#ifdef _BUILD_THREADSAFE_
  return_unlock(leaf, WEED_ERROR_CONCURRENCY);
#endif
  leaf->private_data = data;
  return WEED_SUCCESS;
}

static weed_plant_t *_weed_plant_new(int32_t plant_type) {
  weed_leaf_t *leaf = weed_leaf_new(WEED_LEAF_TYPE, WEED_SEED_INT, WEED_MAGIC_HASH);
  if (!leaf) return NULL;
  leaf->data = weed_data_new(WEED_SEED_INT, 1, &plant_type);
  if (!leaf->data) return weed_leaf_free(leaf);
  leaf->num_elements = 1;
  leaf->flags = WEED_FLAG_IMMUTABLE;
  return leaf;
}

static char **_weed_plant_list_leaves(weed_plant_t *plant, weed_size_t *nleaves) {
  // must use normal malloc, strdup here since the caller will free the strings
  weed_leaf_t *leaf = plant;
  char **leaflist;
  int i = 1, j = 0;
  if (nleaves) *nleaves = 0;
  rwt_upgrade(plant, 0, 0);

  for (; leaf != NULL; i++) leaf = leaf->next;
  if (!(leaflist = (char **)malloc(i * sizeof(char *)))) {
    rwt_unlock(plant);
    return NULL;
  }
  for (leaf = plant; leaf != NULL; leaf = leaf->next) {
    leaflist[j++] = strdup(leaf->key);
    if (!leaflist[j - 1]) {
      rwt_unlock(plant);
      for (--j; j > 0; free(leaflist[--j]));
      free(leaflist);
      return NULL;
    }}
  rwt_unlock(plant);
  leaflist[j] = NULL;
  if (nleaves) *nleaves = j;
  return leaflist;
}

static weed_error_t _weed_leaf_set(weed_plant_t *plant, const char *key,
				   uint32_t seed_type, weed_size_t num_elems,
                                   weed_voidptr_t values) {
  weed_data_t **data = NULL;
  weed_leaf_t *leaf;
  uint32_t hash;
  int isnew = 0;
  weed_size_t old_num_elems = 0;
  weed_data_t **old_data = NULL;

  if (!plant) return WEED_ERROR_NOSUCH_LEAF;
  if (!IS_VALID_SEED_TYPE(seed_type)) return WEED_ERROR_WRONG_SEED_TYPE;

  // lock out other setters
  rwt_upgrade(plant, 0, 0);

  if (!(leaf = weed_find_leaf(plant, key, &hash))) {
    if (!(leaf = weed_leaf_new(key, seed_type, hash))) {
      rwt_unlock(plant);
      return WEED_ERROR_MEMORY_ALLOCATION;
    }
    isnew = 1;
  } else {
    // we hold a readlock on the leaf, we will to grab the mutex and upgrade to a writelock
    // if we fail, too bad the other thread has priority

    if (seed_type != leaf->seed_type) {
      rwt_unlock(plant);
      return_unlock(leaf, WEED_ERROR_WRONG_SEED_TYPE);
    }
    if (leaf->flags & WEED_FLAG_IMMUTABLE) {
      rwt_unlock(plant);
      return_unlock(leaf, WEED_ERROR_IMMUTABLE);
    }
    if (leaf == plant && num_elems != 1) {
      rwt_unlock(plant);
      return_unlock(leaf, WEED_ERROR_NOSUCH_ELEMENT);  ///< type leaf must always have exactly 1 value
    }
    if (rw_upgrade(leaf, 0)) {
      rw_unlock(leaf);
      rwt_unlock(plant);
      return_unlock(leaf, WEED_ERROR_CONCURRENCY);
    }

    rwt_unlock(plant);
    old_num_elems = leaf->num_elements;
    old_data = leaf->data;
  }

  if (num_elems) {
    data = (weed_data_t **)weed_data_new(seed_type, num_elems, values);
    if (!data) {
      // memory failure...
      if (isnew) {
	rwt_unlock(plant);
	weed_leaf_free(leaf);
	leaf = NULL;
      }
      return_unlock(leaf, WEED_ERROR_MEMORY_ALLOCATION);
    }
  }

  leaf->data = data;
  leaf->num_elements = num_elems;

  if (isnew) {
    weed_leaf_append(plant, leaf);
    rwt_unlock(plant);
  }
  else rw_unlock(leaf);

  if (old_data) weed_data_free(old_data, old_num_elems, old_num_elems, seed_type);

  return WEED_SUCCESS;
}

static weed_error_t _weed_leaf_get(weed_plant_t *plant, const char *key, int32_t idx,
				   weed_voidptr_t value) {
  weed_data_t **data;
  uint32_t type;
  weed_leaf_t *leaf;

  if (!(leaf = weed_find_leaf(plant, key, NULL))) {
    return WEED_ERROR_NOSUCH_LEAF;
  }

  if (idx >= leaf->num_elements) {
    return_unlock(leaf, WEED_ERROR_NOSUCH_ELEMENT);
  }
  if (!value) {
    return_unlock(leaf, WEED_SUCCESS);
  }

  type = leaf->seed_type;
  data = leaf->data;

  if (type == WEED_SEED_FUNCPTR) memcpy(value, &(data[idx])->value.funcptr, WEED_FUNCPTR_SIZE);
  else {
    if (weed_seed_is_ptr(type)) memcpy(value, &(data[idx])->value.voidptr, WEED_VOIDPTR_SIZE);
    else {
      if (type == WEED_SEED_STRING) {
	size_t size = (size_t)data[idx]->size;
	char **valuecharptrptr = (char **)value;
	if (size > 0) memcpy(*valuecharptrptr, data[idx]->value.voidptr, size);
	(*valuecharptrptr)[size] = 0;
      } else memcpy(value, data[idx]->value.voidptr, leaf->data[idx]->size);
    }}

  return_unlock(leaf, WEED_SUCCESS);
}

static weed_size_t _weed_leaf_num_elements(weed_plant_t *plant, const char *key) {
  weed_leaf_t *leaf;
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return 0;
  return_unlock(leaf, leaf->num_elements);
}


static weed_size_t _weed_leaf_element_size(weed_plant_t *plant, const char *key, int32_t idx) {
  weed_leaf_t *leaf;
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return 0;
  if (idx > leaf->num_elements) return_unlock(leaf, 0);
  return_unlock(leaf, leaf->data[idx]->size);
}

 static uint32_t _weed_leaf_seed_type(weed_plant_t *plant, const char *key) {
  weed_leaf_t *leaf;
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return WEED_SEED_INVALID;
  return_unlock(leaf, leaf->seed_type);
}

static uint32_t _weed_leaf_get_flags(weed_plant_t *plant, const char *key) {
  weed_leaf_t *leaf;
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return 0;
  return_unlock(leaf, (uint32_t)(leaf->flags & ~WEED_FLAGBITS_RESERVED));
}

static weed_error_t _weed_leaf_get_private_data(weed_plant_t *plant, const char *key,
						void **data_return) {
  weed_leaf_t *leaf;
  if (!(leaf = weed_find_leaf(plant, key, NULL))) return WEED_ERROR_NOSUCH_LEAF;
#ifdef _BUILD_THREADSAFE_
  return_unlock(leaf, WEED_ERROR_CONCURRENCY);
#endif
  if (data_return) data_return = leaf->private_data;
  return WEED_SUCCESS;
}
