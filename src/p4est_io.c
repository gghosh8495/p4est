/*
  This file is part of p4est.
  p4est is a C library to manage a collection (a forest) of multiple
  connected adaptive quadtrees or octrees in parallel.

  Copyright (C) 2010 The University of Texas System
  Additional copyright (C) 2011 individual authors
  Written by Carsten Burstedde, Lucas C. Wilcox, and Tobin Isaac

  p4est is free software; you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation; either version 2 of the License, or
  (at your option) any later version.

  p4est is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with p4est; if not, write to the Free Software Foundation, Inc.,
  51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
*/

#ifndef P4_TO_P8
#include <p4est_algorithms.h>
#include <p4est_bits.h>
#include <p4est_communication.h>
#include <p4est_io.h>
#else
#include <p8est_algorithms.h>
#include <p8est_bits.h>
#include <p8est_communication.h>
#include <p8est_io.h>
#endif
#include <sc_search.h>
#include <sc.h>
#ifndef P4EST_ENABLE_MPII0
#include <errno.h>
#endif

/* error checking macros for p4est_file functions */

/** This macro performs a clean up in the case of a MPI I/O open error.
 * We make use of the fact that sc_mpi_open is always called collectively.
 */
#define P4EST_FILE_CHECK_OPEN(errcode, fc, user_msg, cperrcode) do {\
                                            SC_CHECK_MPI_VERBOSE (errcode, user_msg);   \
                                            *cperrcode = errcode;                       \
                                            if (errcode) {                              \
                                            p4est_file_error_cleanup (&fc->file);       \
                                            P4EST_FREE (fc);                            \
                                            p4est_file_error_code (errcode, cperrcode);\
                                            return NULL;}} while (0)

/** The same as \ref P4EST_FILE_CHECK_OPEN but returns -1 instead of NULL */
#define P4EST_FILE_CHECK_INT(errcode, user_msg, cperrcode) do {\
                                            SC_CHECK_MPI_VERBOSE (errcode, user_msg);   \
                                            *cperrcode = errcode;                       \
                                            if (errcode) {                              \
                                            p4est_file_error_code (errcode, cperrcode);\
                                            return -1;}} while (0)

/** This macro prints the MPI error for sc_mpi_{read,write}_all and return NULL.
 * This means that this macro is appropriate to call it after a collective
 * read or write.
 */
#define P4EST_FILE_CHECK_NULL(errcode, fc, user_msg, cperrcode) do {\
                                            SC_CHECK_MPI_VERBOSE (errcode, user_msg);   \
                                            *cperrcode = errcode;                       \
                                            if (errcode != sc_MPI_SUCCESS) {            \
                                            p4est_file_error_cleanup (&fc->file);       \
                                            P4EST_FREE (fc);                            \
                                            p4est_file_error_code (errcode, cperrcode);\
                                            return NULL;}} while (0)

/** This macro prints the MPI error for sc_mpi_{read,write}.
 * This means that this macro is appropriate to call it after a non-collective
 * read or write. For a correct error handling it is required to skip the rest
 * of the non-collective code and then broadcast the error flag.
 * Can be used only multiple times in a function but will always jump to the
 * same label. This leads to correct error managing.
 */
#define P4EST_FILE_CHECK_MPI(errcode, user_msg) do {SC_CHECK_MPI_VERBOSE (errcode, user_msg); \
                                                        if (mpiret != sc_MPI_SUCCESS) {       \
                                                        goto p4est_read_write_error;}} while (0)

/** Use this macro after \ref P4EST_FILE_CHECK_MPI *directly* after the end of
 * non-collective statements.
 * Can be only used once in a function.
 */
/* Remark: Since we use a declaration after the label we need an empty statement. */
#define P4EST_HANDLE_MPI_ERROR(mpiret,fc,comm,cperrcode) do {p4est_read_write_error: ;             \
                                                    int p4est_mpiret_handle_error =                \
                                                    sc_MPI_Bcast (&mpiret, 1, sc_MPI_INT, 0, comm);\
                                                    SC_CHECK_MPI (p4est_mpiret_handle_error);      \
                                                    *cperrcode = mpiret;                           \
                                                    if (mpiret) {                                  \
                                                    p4est_file_error_cleanup (&fc->file);          \
                                                    P4EST_FREE (fc);                               \
                                                    p4est_file_error_code (mpiret, cperrcode);    \
                                                    return NULL;}} while (0)

/** A macro to check for file write related count errors.
 * These errors are handled as fatal errors. The macro is only applicable for
 * collective calls.
 */
#define P4EST_FILE_CHECK_COUNT(icount,ocount,fc,cperrcode) do { int p4est_count_error_global, p4est_mpiret,\
                                                 p4est_rank;                                               \
                                                 int p4est_file_check_count = ((int) icount != ocount);    \
                                                 p4est_mpiret = sc_MPI_Allreduce (&p4est_file_check_count, \
                                                 &p4est_count_error_global, 1, sc_MPI_INT, sc_MPI_LOR,     \
                                                 fc->mpicomm);                                             \
                                                 SC_CHECK_MPI (p4est_mpiret);                              \
                                                 p4est_mpiret = sc_MPI_Comm_rank (fc->mpicomm, &p4est_rank);\
                                                 SC_CHECK_MPI (p4est_mpiret);                              \
                                                 *cperrcode = (p4est_file_check_count) ?                   \
                                                 P4EST_FILE_COUNT_ERROR : sc_MPI_SUCCESS;                  \
                                                 if (p4est_count_error_global)                             \
                                                 { if (p4est_rank == 0) {                                  \
                                                  SC_LERRORF ("Count error at %s:%d.\n",__FILE__,          \
                                                 __LINE__);}                                               \
                                                 p4est_file_error_cleanup (&fc->file);                     \
                                                 P4EST_FREE (fc);                                          \
                                                 return NULL;}} while (0)

/** A macro to check for file write related count errors. This macro is
 * only applicable for serial calls. The errors are handled as fatal errors.
 * We assume that the macro is called on rank 0.
 */
#define P4EST_FILE_CHECK_COUNT_SERIAL(icount, ocount) do {if (((int) icount) != ocount) {                        \
                                                        SC_LERRORF ("Count error on rank 0 at %s:%d.\n",__FILE__,\
                                                        __LINE__);                                               \
                                                        goto p4est_write_count_error;}} while (0)

/** A macro to handle a file write error that occurred on rank 0 but need to be
 * handled collectivly. We need count_error as input since we need a variable to
 * broadcast the count error status. count_error is true if there is a count error
 * and false otherwise.
 */
/* Remark: Since we use a declaration after the label we need an empty statement. */
#define P4EST_HANDLE_MPI_COUNT_ERROR(count_error,fc,cperrcode) do {p4est_write_count_error: ;\
                                                    int p4est_mpiret_handle = sc_MPI_Bcast (&count_error, 1, sc_MPI_INT, 0,\
                                                    fc->mpicomm);\
                                                    SC_CHECK_MPI (p4est_mpiret_handle);\
                                                    *cperrcode = (count_error) ? P4EST_FILE_COUNT_ERROR : sc_MPI_SUCCESS;\
                                                    if (count_error) {\
                                                    p4est_file_error_cleanup (&fc->file);\
                                                    P4EST_FREE (fc);\
                                                    return NULL;}} while (0)

sc_array_t         *
p4est_deflate_quadrants (p4est_t * p4est, sc_array_t ** data)
{
  const size_t        qsize = sizeof (p4est_qcoord_t);
  const size_t        dsize = p4est->data_size;
  size_t              qtreez, qz;
  sc_array_t         *qarr, *darr;
  p4est_topidx_t      tt;
  p4est_tree_t       *tree;
  p4est_quadrant_t   *q;
  p4est_qcoord_t     *qap;
  char               *dap;

  qarr = sc_array_new_size (qsize,
                            (P4EST_DIM + 1) * p4est->local_num_quadrants);
  qap = (p4est_qcoord_t *) qarr->array;
  darr = NULL;
  dap = NULL;
  if (data != NULL) {
    P4EST_ASSERT (dsize > 0);
    darr = sc_array_new_size (dsize, p4est->local_num_quadrants);
    dap = darr->array;
  }
  for (tt = p4est->first_local_tree; tt <= p4est->last_local_tree; ++tt) {
    tree = p4est_tree_array_index (p4est->trees, tt);
    qtreez = tree->quadrants.elem_count;
    for (qz = 0; qz < qtreez; ++qz) {
      q = p4est_quadrant_array_index (&tree->quadrants, qz);
      *qap++ = q->x;
      *qap++ = q->y;
#ifdef P4_TO_P8
      *qap++ = q->z;
#endif
      *qap++ = (p4est_qcoord_t) q->level;
      if (data != NULL) {
        memcpy (dap, q->p.user_data, dsize);
        dap += dsize;
      }
    }
  }
  P4EST_ASSERT ((void *) qap ==
                qarr->array + qarr->elem_size * qarr->elem_count);
  if (data != NULL) {
    P4EST_ASSERT (dap == darr->array + darr->elem_size * darr->elem_count);
    *data = darr;
  }
  return qarr;
}

p4est_t            *
p4est_inflate (sc_MPI_Comm mpicomm, p4est_connectivity_t * connectivity,
               const p4est_gloidx_t * global_first_quadrant,
               const p4est_gloidx_t * pertree,
               sc_array_t * quadrants, sc_array_t * data, void *user_pointer)
{
  const p4est_gloidx_t *gfq;
  int                 i;
  int                 num_procs, rank;
  p4est_topidx_t      num_trees, jt;
  p4est_gloidx_t      gkey, gtreeskip, gtreeremain, gquadremain;
  p4est_t            *p4est;
  p4est_tree_t       *tree;
  p4est_quadrant_t   *q;
#ifdef P4EST_ENABLE_DEBUG
  int                 p;
#endif
  int8_t              ql, tml;
  size_t              dsize;
  size_t              gk1, gk2;
  size_t              qz, zqoffset, zqthistree;
  p4est_qcoord_t     *qap;
  char               *dap;

  P4EST_GLOBAL_PRODUCTION ("Into " P4EST_STRING "_inflate\n");
  p4est_log_indent_push ();

  P4EST_ASSERT (p4est_connectivity_is_valid (connectivity));
  P4EST_ASSERT (global_first_quadrant != NULL);
  P4EST_ASSERT (pertree != NULL);
  P4EST_ASSERT (quadrants != NULL);
  P4EST_ASSERT (quadrants->elem_size == sizeof (p4est_qcoord_t));
  /* data may be NULL, in this case p4est->data_size will be 0 */
  /* user_pointer may be anything, we don't look at it */

  /* create p4est object and assign some data members */
  p4est = P4EST_ALLOC_ZERO (p4est_t, 1);
  dsize = p4est->data_size = (data == NULL ? 0 : data->elem_size);
  dap = (char *) (data == NULL ? NULL : data->array);
  qap = (p4est_locidx_t *) quadrants->array;
  p4est->user_pointer = user_pointer;
  p4est->connectivity = connectivity;
  num_trees = connectivity->num_trees;

  /* set parallel environment */
  p4est_comm_parallel_env_assign (p4est, mpicomm);
  num_procs = p4est->mpisize;
  rank = p4est->mpirank;

  /* create global first quadrant offsets */
  gfq = p4est->global_first_quadrant =
    P4EST_ALLOC (p4est_gloidx_t, num_procs + 1);
  memcpy (p4est->global_first_quadrant, global_first_quadrant,
          (num_procs + 1) * sizeof (p4est_gloidx_t));
#ifdef P4EST_ENABLE_DEBUG
  P4EST_ASSERT (gfq[0] == 0);
  for (p = 0; p < num_procs; ++p) {
    P4EST_ASSERT (gfq[p] <= gfq[p + 1]);
  }
  P4EST_ASSERT (pertree[0] == 0);
  for (jt = 0; jt < num_trees; ++jt) {
    P4EST_ASSERT (pertree[jt] <= pertree[jt + 1]);
  }
  P4EST_ASSERT (gfq[num_procs] == pertree[num_trees]);
#endif
  gquadremain = gfq[rank + 1] - gfq[rank];
  p4est->local_num_quadrants = (p4est_locidx_t) gquadremain;
  p4est->global_num_quadrants = gfq[num_procs];
  P4EST_ASSERT (quadrants->elem_count ==
                (P4EST_DIM + 1) * (size_t) p4est->local_num_quadrants);
  P4EST_ASSERT (data == NULL || data->elem_count ==
                (size_t) p4est->local_num_quadrants);

  /* allocate memory pools */
  if (dsize > 0) {
    p4est->user_data_pool = sc_mempool_new (dsize);
  }
  else {
    p4est->user_data_pool = NULL;
  }
  p4est->quadrant_pool = sc_mempool_new (sizeof (p4est_quadrant_t));

  /* find the first and last tree on this processor */
  if (p4est->local_num_quadrants > 0) {
    gkey = gfq[rank];
    gk1 = sc_bsearch_range (&gkey, pertree, num_trees,
                            sizeof (p4est_gloidx_t), p4est_gloidx_compare);
    P4EST_ASSERT (gk1 < (size_t) num_trees);
    gtreeskip = gkey - pertree[gk1];
    gkey = gfq[rank + 1] - 1;
    gk2 = sc_bsearch_range (&gkey, pertree, num_trees,
                            sizeof (p4est_gloidx_t), p4est_gloidx_compare);
    P4EST_ASSERT (gk1 <= gk2 && gk2 < (size_t) num_trees);
    p4est->first_local_tree = (p4est_topidx_t) gk1;
    p4est->last_local_tree = (p4est_topidx_t) gk2;
  }
  else {
    gtreeskip = 0;
    p4est->first_local_tree = -1;
    p4est->last_local_tree = -2;
  }

  /* populate trees */
  zqoffset = 0;
  gquadremain = p4est->local_num_quadrants;
  p4est->trees = sc_array_new_size (sizeof (p4est_tree_t), num_trees);
  for (jt = 0; jt < num_trees; ++jt) {
    /* all trees need at least some basic setup */
    tree = p4est_tree_array_index (p4est->trees, jt);
    sc_array_init (&tree->quadrants, sizeof (p4est_quadrant_t));
    P4EST_QUADRANT_INIT (&tree->first_desc);
    P4EST_QUADRANT_INIT (&tree->last_desc);
    tree->quadrants_offset = (p4est_locidx_t) zqoffset;
    for (i = 0; i <= P4EST_QMAXLEVEL; ++i) {
      tree->quadrants_per_level[i] = 0;
    }
    for (; i <= P4EST_MAXLEVEL; ++i) {
      tree->quadrants_per_level[i] = -1;
    }
    q = NULL;
    tree->maxlevel = 0;
    if (jt >= p4est->first_local_tree && jt <= p4est->last_local_tree) {
      /* this tree has local quadrants */
      gtreeremain = pertree[jt + 1] - pertree[jt] - gtreeskip;
      P4EST_ASSERT (gtreeremain > 0 && gquadremain > 0);
      zqthistree = (size_t) SC_MIN (gtreeremain, gquadremain);
      P4EST_ASSERT (zqthistree > 0);
      sc_array_resize (&tree->quadrants, zqthistree);
      tml = 0;
      for (qz = 0; qz < zqthistree; ++qz) {
        q = p4est_quadrant_array_index (&tree->quadrants, qz);
        P4EST_QUADRANT_INIT (q);
        q->x = *qap++;
        q->y = *qap++;
#ifdef P4_TO_P8
        q->z = *qap++;
#endif
/* *INDENT-OFF* HORRIBLE indent bug */
        q->level = ql = (int8_t) *qap++;
/* *INDENT-ON* */
        P4EST_ASSERT (ql >= 0 && ql <= P4EST_QMAXLEVEL);
        ++tree->quadrants_per_level[ql];
        tml = SC_MAX (tml, ql);
        p4est_quadrant_init_data (p4est, jt, q, NULL);
        if (data != NULL) {
          memcpy (q->p.user_data, dap, dsize);
          dap += dsize;
        }
        if (qz == 0) {
          p4est_quadrant_first_descendant (q, &tree->first_desc,
                                           P4EST_QMAXLEVEL);
        }
      }
      p4est_quadrant_last_descendant (q, &tree->last_desc, P4EST_QMAXLEVEL);
      tree->maxlevel = tml;
      zqoffset += zqthistree;
      gquadremain -= (p4est_gloidx_t) zqthistree;
      gtreeskip = 0;
    }
  }
  P4EST_ASSERT (zqoffset == (size_t) p4est->local_num_quadrants);
  P4EST_ASSERT (gquadremain == 0);

  /* communicate partition information */
  p4est->global_first_position =
    P4EST_ALLOC (p4est_quadrant_t, num_procs + 1);
  p4est_comm_global_partition (p4est, NULL);

  /* print more statistics */
  P4EST_VERBOSEF ("total local quadrants %lld\n",
                  (long long) p4est->local_num_quadrants);

  P4EST_ASSERT (p4est->revision == 0);
  P4EST_ASSERT (p4est_is_valid (p4est));
  p4est_log_indent_pop ();
  P4EST_GLOBAL_PRODUCTION ("Done " P4EST_STRING "_inflate\n");

  return p4est;
}

/* Avoid redefinition in p4est_to_p8est.h */
#ifdef P4_TO_P8
#define p4est_file_context               p8est_file_context
#endif

/** The opaque file context for for p4est data files. */
struct p4est_file_context
{
  sc_MPI_Comm         mpicomm;            /**< corresponding MPI communicator */
  p4est_locidx_t      local_num_quadrants; /**< number of local quadrants */
  p4est_gloidx_t      global_num_quadrants; /**< number of global quadrants */
  p4est_gloidx_t     *global_first_quadrant; /**< represents the partition */
  int                 gfq_owned;          /**< Boolean to indicate if global_first_quadrant
                                               is owned. */
  size_t              num_calls;        /**< redundant but for convenience;
                                            counts the number of calls of
                                            write and read, respectively */
  sc_MPI_File         file;
  sc_MPI_Offset       accessed_bytes;   /**< count only array data bytes and
                                           array metadata bytes */
};

/** This function calculates a padding string consisting of spaces.
 * We require an already allocated array pad or NULL.
 * The number of bytes in pad must be at least divisor!
 * For NULL the function only calculates the number of padding bytes.
 */
static void
get_padding_string (size_t num_bytes, size_t divisor, char *pad,
                    size_t * num_pad_bytes)
{
  P4EST_ASSERT (divisor != 0 && num_pad_bytes != NULL);

  *num_pad_bytes = (divisor - (num_bytes % divisor)) % divisor;
  if (*num_pad_bytes == 0 || *num_pad_bytes == 1) {
    /* In these cases there is no space to add new line characters
     * but this is necessary to ensure a consistent layout in a text editor
     */
    *num_pad_bytes += divisor;
  }

  P4EST_ASSERT (*num_pad_bytes > 1);
  if (pad != NULL) {
    snprintf (pad, *num_pad_bytes + 1, "\n%-*s\n", (int) *num_pad_bytes - 2,
              "");
  }
}

static int
check_file_metadata (sc_MPI_Comm mpicomm, const char *filename,
                     char user_string[P4EST_NUM_USER_STRING_BYTES],
                     char *metadata, p4est_gloidx_t * global_num_quadrants)
{
  long                read_global_num_quads;
  int                 mpiret, rank;
  int                 error_flag;

  P4EST_ASSERT (metadata != NULL);

  error_flag = 0;

  mpiret = sc_MPI_Comm_rank (mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

  /* check magic number */
  if (metadata[P4EST_NUM_MAGIC_BYTES - 1] != '\n') {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong file header format.\n");
    }
    return P4EST_ERR_IO;
  }

  metadata[P4EST_NUM_MAGIC_BYTES - 1] = '\0';
  if (strcmp (metadata, P4EST_MAGIC_NUMBER)) {
    /* TODO: check for wrong endianness */
    P4EST_LERRORF (P4EST_STRING
                   "_io: Error reading <%s>. Wrong magic number (in file = %s, magic number = %s).\n",
                   filename, metadata, P4EST_MAGIC_NUMBER);
    error_flag = 1;
  }

  /* check format of version string line */
  if (metadata[P4EST_NUM_MAGIC_BYTES + P4EST_NUM_VERSION_STR_BYTES - 1] !=
      '\n') {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong file header format.\n");
    }
    return P4EST_ERR_IO;
  }

  metadata[P4EST_NUM_MAGIC_BYTES + P4EST_NUM_VERSION_STR_BYTES - 1] = '\0';
  if (strlen (&metadata[P4EST_NUM_MAGIC_BYTES]) !=
      P4EST_NUM_VERSION_STR_BYTES - 1) {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong file header format.\n");
    }
    return P4EST_ERR_IO;
  }

  /* check the format of the user string */
  if (metadata
      [P4EST_NUM_MAGIC_BYTES + P4EST_NUM_VERSION_STR_BYTES +
       P4EST_NUM_USER_STRING_BYTES - 1] != '\n') {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong file header format.\n");
    }
    return P4EST_ERR_IO;
  }
  /* the content of the user string is not checked */
  strncpy (user_string,
           &metadata[P4EST_NUM_MAGIC_BYTES + P4EST_NUM_VERSION_STR_BYTES],
           P4EST_NUM_USER_STRING_BYTES - 1);
  user_string[P4EST_NUM_USER_STRING_BYTES - 1] = '\0';

  /* check number of global quadrants */
  /* there is no \n at the end of this line */
  metadata[P4EST_NUM_METADATA_BYTES] = '\0';

  if (strlen
      (&metadata
       [P4EST_NUM_MAGIC_BYTES + P4EST_NUM_VERSION_STR_BYTES +
        P4EST_NUM_USER_STRING_BYTES]) != 16) {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong file header format.\n");
    }
    return P4EST_ERR_IO;
  }

  read_global_num_quads =
    sc_atol (&metadata
             [P4EST_NUM_MAGIC_BYTES + P4EST_NUM_VERSION_STR_BYTES +
              P4EST_NUM_USER_STRING_BYTES]);
  *global_num_quadrants = (p4est_gloidx_t) read_global_num_quads;
  if (read_global_num_quads < 0) {
    P4EST_LERRORF (P4EST_STRING
                   "_io: Error reading <%s>. Negative global number of quadrants.\n",
                   filename);
    error_flag = 1;
  }

  return (error_flag) ? P4EST_ERR_IO : sc_MPI_SUCCESS;
}

/** Close an MPI file or its libsc-internal replacement in case of an error.
 * \param [in,out]  file    A sc_MPI_file
 * \return                  Always -1 since this function is only called
 *                          if an error already occurred.
 */
static int
p4est_file_error_cleanup (sc_MPI_File * file)
{
  /* no error checking since we are called under an error condition */
  P4EST_ASSERT (file != NULL);
#ifdef P4EST_ENABLE_MPIIO
  if (*file != sc_MPI_FILE_NULL) {
#else
  if ((*file)->file != sc_MPI_FILE_NULL) {
#endif
    /* We do not use here the libsc closing function since we do not perform
     * error checking in this function that is only called if we had already
     * an error.
     */
#ifdef P4EST_ENABLE_MPIIO
    MPI_File_close (file);
#else
    {
#ifdef P4EST_ENABLE_MPI
      int                 rank, mpiret;
#endif

#ifdef P4EST_ENABLE_MPI
      mpiret = sc_MPI_Comm_rank ((*file)->mpicomm, &rank);
      SC_CHECK_MPI (mpiret);

      if (rank == 0) {
#endif
        fclose ((*file)->file);
        (*file)->file = NULL;
#ifdef P4EST_ENABLE_MPI
      }
#endif
    }
#endif
  }
#ifndef P4EST_ENABLE_MPIIO
  SC_FREE (*file);
#endif
  return -1;
}

static int          p4est_file_error_code (int errcode, int *p4est_errcode);

p4est_file_context_t *
p4est_file_open_create (p4est_t * p4est, const char *filename,
                        const char user_string[P4EST_NUM_USER_STRING_BYTES],
                        int *errcode)
{
  int                 mpiret, count, count_error, mpisize;
  /* We enforce the padding of the file header. */
  char                metadata[P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV + 1];
  p4est_file_context_t *file_context = P4EST_ALLOC (p4est_file_context_t, 1);

  P4EST_ASSERT (p4est_is_valid (p4est));
  P4EST_ASSERT (filename != NULL);
  P4EST_ASSERT (errcode != NULL);
  P4EST_ASSERT (strlen (user_string) < P4EST_NUM_USER_STRING_BYTES);

  /* Open the file and create a new file if necessary */
  mpiret =
    sc_io_open (p4est->mpicomm, filename,
                SC_WRITE_CREATE, sc_MPI_INFO_NULL, &file_context->file);
  P4EST_FILE_CHECK_OPEN (mpiret, file_context, "File open create", errcode);

  if (p4est->mpirank == 0) {
    /* write padded p4est-defined header */
    snprintf (metadata, P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV + 1,
              "%.7s\n%-23s\n%-47s\n%.16lld\n%-14s\n", P4EST_MAGIC_NUMBER,
              p4est_version (), user_string,
              (long long) p4est->global_num_quadrants, "");
    mpiret =
      sc_io_write_at (file_context->file, 0, metadata,
                      P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV, sc_MPI_BYTE,
                      &count);
    P4EST_FILE_CHECK_MPI (mpiret, "Writing the file header");
    count_error = (P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV,
                                   count);
  }

  P4EST_HANDLE_MPI_ERROR (mpiret, file_context, p4est->mpicomm, errcode);

  /* initialize the file context */
  file_context->mpicomm = p4est->mpicomm;
  file_context->local_num_quadrants = p4est->local_num_quadrants;
  file_context->global_num_quadrants = p4est->global_num_quadrants;
  mpiret = sc_MPI_Comm_size (p4est->mpicomm, &mpisize);
  file_context->global_first_quadrant =
    P4EST_ALLOC (p4est_gloidx_t, mpisize + 1);
  memcpy (file_context->global_first_quadrant, p4est->global_first_quadrant,
          (mpisize + 1) * sizeof (p4est_gloidx_t));
  file_context->gfq_owned = 1;

  P4EST_HANDLE_MPI_COUNT_ERROR (count_error, file_context, errcode);

  file_context->accessed_bytes = 0;
  file_context->num_calls = 0;

  p4est_file_error_code (*errcode, errcode);
  return file_context;
}

p4est_file_context_t *
p4est_file_open_read_ext (sc_MPI_Comm mpicomm, const char *filename,
                          char user_string[P4EST_NUM_USER_STRING_BYTES],
                          p4est_gloidx_t * global_num_quadrants, int *errcode)
{
  int                 mpiret, rank;
  int                 count, count_error;
  char                metadata[P4EST_NUM_METADATA_BYTES + 1];
  p4est_file_context_t *file_context = P4EST_ALLOC (p4est_file_context_t, 1);

  P4EST_ASSERT (filename != NULL);
  P4EST_ASSERT (user_string != NULL);
  P4EST_ASSERT (global_num_quadrants != NULL);
  P4EST_ASSERT (errcode != NULL);

  /* Open the file in the reading mode */
  mpiret =
    sc_io_open (mpicomm, filename, SC_READ,
                sc_MPI_INFO_NULL, &file_context->file);
  P4EST_FILE_CHECK_OPEN (mpiret, file_context, "File open read", errcode);

  file_context->mpicomm = mpicomm;
  file_context->local_num_quadrants = 0;        /* not set for read calls */
  file_context->global_first_quadrant = NULL;
  file_context->gfq_owned = 0;
  file_context->accessed_bytes = 0;
  file_context->num_calls = 0;

  /* get the MPI rank */
  mpiret = sc_MPI_Comm_rank (mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

  /* read metadata and deallocate in case of error */
  if (rank == 0) {
    /* read metadata on rank 0 */
    mpiret =
      sc_io_read_at (file_context->file, 0, metadata,
                     P4EST_NUM_METADATA_BYTES, sc_MPI_BYTE, &count);

    P4EST_FILE_CHECK_MPI (mpiret, "Reading metadata");
    count_error = (P4EST_NUM_METADATA_BYTES != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (P4EST_NUM_METADATA_BYTES, count);

    metadata[P4EST_NUM_METADATA_BYTES] = '\0';
    /* parse metadata; we do not use file_info because we do not want a Bcast */
    mpiret =
      check_file_metadata (mpicomm, filename, user_string, metadata,
                           global_num_quadrants);
    P4EST_FILE_CHECK_MPI (mpiret, "Check file header");
  }

  /* error checking */
  P4EST_HANDLE_MPI_ERROR (mpiret, file_context, mpicomm, errcode);
  P4EST_HANDLE_MPI_COUNT_ERROR (count_error, file_context, errcode);

  /* broadcast the user string of the file */
  mpiret =
    sc_MPI_Bcast (user_string, P4EST_NUM_USER_STRING_BYTES, sc_MPI_BYTE, 0,
                  mpicomm);
  SC_CHECK_MPI (mpiret);

  /* broadcast the number of global quadrants */
  mpiret =
    sc_MPI_Bcast (global_num_quadrants, sizeof (p4est_gloidx_t), sc_MPI_BYTE,
                  0, mpicomm);
  SC_CHECK_MPI (mpiret);

  file_context->global_num_quadrants = *global_num_quadrants;

  p4est_file_error_code (*errcode, errcode);
  return file_context;
}

p4est_file_context_t *
p4est_file_open_read (p4est_t * p4est, const char *filename,
                      char user_string[P4EST_NUM_USER_STRING_BYTES],
                      int *errcode)
{
  p4est_gloidx_t      global_num_quadrants;
  p4est_file_context_t *fc;

  P4EST_ASSERT (p4est_is_valid (p4est));
  P4EST_ASSERT (filename != NULL);
  P4EST_ASSERT (user_string != NULL);
  P4EST_ASSERT (errcode != NULL);

  fc =
    p4est_file_open_read_ext (p4est->mpicomm, filename, user_string,
                              &global_num_quadrants, errcode);

  /* check global number of quadrants */
  if (fc != NULL && p4est->global_num_quadrants != global_num_quadrants) {
    if (p4est->mpirank == 0) {
      P4EST_LERRORF (P4EST_STRING "_file_open_read: global number of "
                     "quadrants mismatch (in file = %ld,"
                     " by parameter = %ld)\n", global_num_quadrants,
                     p4est->global_num_quadrants);
    }
    p4est_file_close (fc, errcode);
    P4EST_FILE_CHECK_NULL (*errcode, fc,
                           P4EST_STRING "_file_open_read: close file",
                           errcode);
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }

  if (fc != NULL) {
    /* use the partition of the given p4est */
    fc->global_first_quadrant = p4est->global_first_quadrant;
    fc->gfq_owned = 0;
  }

  p4est_file_error_code (*errcode, errcode);
  return fc;
}

p4est_file_context_t *
p4est_file_write_header (p4est_file_context_t * fc, size_t header_size,
                         const void *header_data,
                         const char user_string[P4EST_NUM_USER_STRING_BYTES],
                         int *errcode)
{
  size_t              num_pad_bytes;
  char                header_metadata[P4EST_NUM_FIELD_HEADER_BYTES + 1],
    pad[P4EST_MAX_NUM_PAD_BYTES];
  int                 mpiret, count, count_error, rank;

  P4EST_ASSERT (fc != NULL);
  P4EST_ASSERT (fc->global_first_quadrant != NULL);
  P4EST_ASSERT (header_data != NULL);
  P4EST_ASSERT (errcode != NULL);
  P4EST_ASSERT (strlen (user_string) < P4EST_NUM_USER_STRING_BYTES);

  if (header_size == 0) {
    /* nothing to write */
    *errcode = sc_MPI_SUCCESS;
    p4est_file_error_code (*errcode, errcode);
    return fc;
  }

  mpiret = sc_MPI_Comm_rank (fc->mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

#ifdef P4EST_ENABLE_MPIIO
  /* set the file size */
  mpiret = MPI_File_set_size (fc->file,
                              P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV +
                              header_size +
                              P4EST_NUM_FIELD_HEADER_BYTES +
                              fc->accessed_bytes);
  P4EST_FILE_CHECK_NULL (mpiret, fc, "Set file size", errcode);
#else
  /* We do not perform this optimization without MPI I/O */
#endif

  num_pad_bytes = 0;
  if (rank == 0) {
    /* header-dependent metadata */
    snprintf (header_metadata,
              P4EST_NUM_FIELD_HEADER_BYTES +
              1, "H %.13llu\n%-47s\n", (unsigned long long) header_size,
              user_string);

    /* write header-dependent metadata */
    mpiret =
      sc_io_write_at (fc->file,
                      fc->accessed_bytes + P4EST_NUM_METADATA_BYTES +
                      P4EST_BYTE_DIV, header_metadata,
                      P4EST_NUM_FIELD_HEADER_BYTES, sc_MPI_BYTE, &count);

    P4EST_FILE_CHECK_MPI (mpiret, "Writing header metadata");
    count_error = (P4EST_NUM_FIELD_HEADER_BYTES != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (P4EST_NUM_FIELD_HEADER_BYTES, count);
  }
  P4EST_HANDLE_MPI_ERROR (mpiret, fc, fc->mpicomm, errcode);
  P4EST_HANDLE_MPI_COUNT_ERROR (count_error, fc, errcode);

  /*write header data */
  if (rank == 0) {
    mpiret =
      sc_io_write_at (fc->file,
                      fc->accessed_bytes + P4EST_NUM_METADATA_BYTES +
                      P4EST_BYTE_DIV + P4EST_NUM_FIELD_HEADER_BYTES,
                      header_data, header_size, sc_MPI_BYTE, &count);

    P4EST_FILE_CHECK_MPI (mpiret, "Writing header data");
    count_error = ((int) header_size != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (header_size, count);

    /* write padding bytes */
    get_padding_string (header_size, P4EST_BYTE_DIV, pad, &num_pad_bytes);
    mpiret =
      sc_io_write_at (fc->file,
                      fc->accessed_bytes + P4EST_NUM_METADATA_BYTES +
                      P4EST_BYTE_DIV + P4EST_NUM_FIELD_HEADER_BYTES +
                      header_size, pad, num_pad_bytes, sc_MPI_BYTE, &count);
    P4EST_FILE_CHECK_MPI (mpiret, "Writing padding bytes for header data");
    count_error = ((int) num_pad_bytes != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (num_pad_bytes, count);
  }
  else {
    get_padding_string (header_size, P4EST_BYTE_DIV, NULL, &num_pad_bytes);
  }

  /* This is *not* the processor local value */
  fc->accessed_bytes +=
    header_size + P4EST_NUM_FIELD_HEADER_BYTES + num_pad_bytes;
  ++fc->num_calls;

  p4est_file_error_code (*errcode, errcode);
  return fc;
}

/** Collectivly read and check block metadata.
 * If user_string == NULL data_size is not compared to
 * read_data_size.
 */
static p4est_file_context_t *
read_block_metadata (p4est_file_context_t * fc, size_t * read_data_size,
                     size_t data_size, char block_type,
                     char user_string[P4EST_NUM_USER_STRING_BYTES],
                     int *errcode)
{
  int                 mpiret, count, count_error, rank;
  int                 bytes_to_read;
  int                 err_flag;
  char                block_metadata[P4EST_NUM_FIELD_HEADER_BYTES];
  size_t              data_block_size, num_pad_bytes;

  P4EST_ASSERT (read_data_size != NULL);
  P4EST_ASSERT (errcode != NULL);

  mpiret = sc_MPI_Comm_rank (fc->mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

  bytes_to_read =
    (user_string !=
     NULL) ? P4EST_NUM_FIELD_HEADER_BYTES : (P4EST_NUM_ARRAY_METADATA_BYTES +
                                             2);
  if (rank == 0) {
    mpiret = sc_io_read_at (fc->file,
                            fc->accessed_bytes +
                            P4EST_NUM_METADATA_BYTES +
                            P4EST_BYTE_DIV, block_metadata,
                            bytes_to_read, sc_MPI_BYTE, &count);
    P4EST_FILE_CHECK_MPI (mpiret, "Reading data section-wise metadata");
    count_error = (bytes_to_read != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (bytes_to_read, count);
  }
  /* In the case of error the return value is still NULL */
  P4EST_HANDLE_MPI_ERROR (mpiret, fc, fc->mpicomm, errcode);
  P4EST_HANDLE_MPI_COUNT_ERROR (count_error, fc, errcode);

  /* broadcast block metadata to calculate correct internals on each rank */
  mpiret =
    sc_MPI_Bcast (block_metadata, bytes_to_read, sc_MPI_BYTE, 0, fc->mpicomm);
  SC_CHECK_MPI (mpiret);

  /* check for given block specifying character */
  if (block_metadata[0] != block_type) {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong data section type.\n");
    }
    p4est_file_error_cleanup (&fc->file);
    P4EST_FREE (fc);
    *errcode = P4EST_ERR_IO;
    return NULL;
  }

  /* check '\n' to check the format */
  if (block_metadata[P4EST_NUM_ARRAY_METADATA_BYTES + 1] != '\n') {
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong section header format.\n");
    }
    p4est_file_error_cleanup (&fc->file);
    P4EST_FREE (fc);
    *errcode = P4EST_ERR_IO;
    return NULL;
  }

  /* process the block metadata */
  block_metadata[P4EST_NUM_ARRAY_METADATA_BYTES + 1] = '\0';
  /* we cut off the block type specifier */
  *read_data_size = sc_atol (&block_metadata[2]);

  if (user_string != NULL && *read_data_size != data_size) {
    if (rank == 0) {
      P4EST_LERRORF (P4EST_STRING
                     "_io: Error reading. Wrong array element size (in file = %ld, by parameter = %ld).\n",
                     *read_data_size, data_size);
    }
    p4est_file_error_cleanup (&fc->file);
    P4EST_FREE (fc);
    *errcode = P4EST_ERR_IO;
    return NULL;
  }

  if (user_string != NULL) {
    /* check '\n' to check the format */
    if (block_metadata[P4EST_NUM_FIELD_HEADER_BYTES - 1] != '\n') {
      if (rank == 0) {
        P4EST_LERROR (P4EST_STRING
                      "_io: Error reading. Wrong section header format.\n");
      }
      p4est_file_error_cleanup (&fc->file);
      P4EST_FREE (fc);
      *errcode = P4EST_ERR_IO;
      return NULL;
    }

    /* null-terminate the user string of the current block */
    block_metadata[P4EST_NUM_FIELD_HEADER_BYTES - 1] = '\0';

    /* copy the user string, '\0' was already set above */
    strncpy (user_string, &block_metadata[P4EST_NUM_ARRAY_METADATA_BYTES + 2],
             P4EST_NUM_USER_STRING_BYTES);
    P4EST_ASSERT (user_string[P4EST_NUM_USER_STRING_BYTES - 1] == '\0');
  }

  /* check the padding structure */
  err_flag = 0;
  if (rank == 0) {
    /* calculate number of padding bytes */
    if (block_metadata[0] == 'F') {
      data_block_size = *read_data_size * fc->global_num_quadrants;
    }
    else if (block_metadata[0] == 'H') {
      data_block_size = *read_data_size;
    }
    else {
      /* We assume that this function is called for a valid block type. */
      SC_ABORT_NOT_REACHED ();
    }
    get_padding_string (data_block_size, P4EST_BYTE_DIV, NULL,
                        &num_pad_bytes);
    /* read padding bytes */
    mpiret = sc_io_read_at (fc->file,
                            fc->accessed_bytes +
                            P4EST_NUM_METADATA_BYTES +
                            P4EST_BYTE_DIV + P4EST_NUM_FIELD_HEADER_BYTES +
                            data_block_size, block_metadata, num_pad_bytes,
                            sc_MPI_BYTE, &count);
    P4EST_FILE_CHECK_MPI (mpiret, "Reading padding bytes");
    count_error = ((int) num_pad_bytes != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (num_pad_bytes, count);
    /* check '\n' in padding bytes */
    if (block_metadata[0] != '\n'
        || block_metadata[num_pad_bytes - 1] != '\n') {
      err_flag = 1;
    }
  }
  /* broadcast error status */
  mpiret = sc_MPI_Bcast (&err_flag, 1, sc_MPI_INT, 0, fc->mpicomm);
  SC_CHECK_MPI (mpiret);

  if (err_flag) {
    /* wrong padding format */
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. Wrong padding format.\n");
    }
    p4est_file_error_cleanup (&fc->file);
    P4EST_FREE (fc);
    *errcode = P4EST_ERR_IO;
    return NULL;
  }

  return fc;
}

p4est_file_context_t *
p4est_file_read_header (p4est_file_context_t * fc,
                        size_t header_size, void *header_data,
                        char user_string[P4EST_NUM_USER_STRING_BYTES],
                        int *errcode)
{
  int                 mpiret, count, count_error, rank;
  size_t              num_pad_bytes, read_data_size;
#ifdef P4EST_ENABLE_MPIIO
  sc_MPI_Offset       size;
#endif

  P4EST_ASSERT (fc != NULL);
  P4EST_ASSERT (errcode != NULL);

  mpiret = sc_MPI_Comm_rank (fc->mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

  num_pad_bytes = 0;
  /* calculate the padding bytes for this header data */
  get_padding_string (header_size, P4EST_BYTE_DIV, NULL, &num_pad_bytes);
  if (header_data == NULL || header_size == 0) {
    /* Nothing to read but we shift our own file pointer */
    if (read_block_metadata (fc, &read_data_size, 0, 'H', NULL, errcode) ==
        NULL) {
      p4est_file_error_code (*errcode, errcode);
      return NULL;
    }

    fc->accessed_bytes +=
      header_size + P4EST_NUM_FIELD_HEADER_BYTES + num_pad_bytes;
    ++fc->num_calls;
    *errcode = sc_MPI_SUCCESS;
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }

#ifdef P4EST_ENABLE_MPIIO
  /* check file size; no sync required because the file size does not
   * change in the reading mode.
   */
  mpiret = MPI_File_get_size (fc->file, &size);
  P4EST_FILE_CHECK_NULL (mpiret, fc, "Get file size for read", errcode);
  if (((size_t) size) - P4EST_NUM_METADATA_BYTES - P4EST_BYTE_DIV -
      P4EST_NUM_FIELD_HEADER_BYTES < header_size) {
    /* report wrong file size, collectively close the file and deallocate fc */
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. File has less bytes than the user wants to read.\n");
    }
    mpiret = p4est_file_close (fc, &mpiret);
    P4EST_FILE_CHECK_NULL (mpiret, fc,
                           P4EST_STRING "_file_read_data: close file",
                           errcode);
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }
#else
  /* There is no C-standard functionality to get the file size */
#endif

  /* check the header metadata */
  if (read_block_metadata
      (fc, &read_data_size, header_size, 'H', user_string, errcode) == NULL) {
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }

  if (rank == 0) {
    mpiret = sc_io_read_at (fc->file, fc->accessed_bytes +
                            P4EST_NUM_METADATA_BYTES +
                            P4EST_NUM_FIELD_HEADER_BYTES +
                            P4EST_BYTE_DIV, header_data, (int) header_size,
                            sc_MPI_BYTE, &count);
    P4EST_FILE_CHECK_MPI (mpiret, "Reading header data");
    count_error = ((int) header_size != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (header_size, count);
  }
  P4EST_HANDLE_MPI_ERROR (mpiret, fc, fc->mpicomm, errcode);
  P4EST_HANDLE_MPI_COUNT_ERROR (count_error, fc, errcode);

  /* broadcast the header data */
  mpiret =
    sc_MPI_Bcast (header_data, header_size, sc_MPI_BYTE, 0, fc->mpicomm);
  SC_CHECK_MPI (mpiret);

  fc->accessed_bytes +=
    header_size + P4EST_NUM_FIELD_HEADER_BYTES + num_pad_bytes;
  ++fc->num_calls;

  p4est_file_error_code (*errcode, errcode);
  return fc;
}

p4est_file_context_t *
p4est_file_write_field (p4est_file_context_t * fc, sc_array_t * quadrant_data,
                        const char user_string[P4EST_NUM_USER_STRING_BYTES],
                        int *errcode)
{
  size_t              bytes_to_write, num_pad_bytes, array_size;
  char                array_metadata[P4EST_NUM_FIELD_HEADER_BYTES + 1],
    pad[P4EST_MAX_NUM_PAD_BYTES];
  sc_MPI_Offset       write_offset;
  int                 mpiret, count, count_error, rank;

  P4EST_ASSERT (fc != NULL);
  P4EST_ASSERT (quadrant_data != NULL
                && quadrant_data->elem_count ==
                (size_t) fc->local_num_quadrants);
  P4EST_ASSERT (errcode != NULL);
  P4EST_ASSERT (strlen (user_string) < P4EST_NUM_USER_STRING_BYTES);

  mpiret = sc_MPI_Comm_rank (fc->mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

  if (quadrant_data->elem_size == 0) {
    /* nothing to write */
    *errcode = sc_MPI_SUCCESS;
    p4est_file_error_code (*errcode, errcode);
    return fc;
  }

  /* Check how many bytes we write to the disk */
  bytes_to_write = quadrant_data->elem_count * quadrant_data->elem_size;

  /* rank-dependent byte offset */
  write_offset = P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV +
    fc->global_first_quadrant[rank] * quadrant_data->elem_size;

#ifdef P4EST_ENABLE_MPIIO
  /* set the file size */
  mpiret = MPI_File_set_size (fc->file,
                              P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV +
                              fc->global_num_quadrants *
                              quadrant_data->elem_size +
                              P4EST_NUM_FIELD_HEADER_BYTES +
                              fc->accessed_bytes);
  P4EST_FILE_CHECK_NULL (mpiret, fc, "Set file size", errcode);
#else
  /* We do not perform this optimization without MPI I/O */
#endif

  num_pad_bytes = 0;
  if (rank == 0) {
    /* array-dependent metadata */
    snprintf (array_metadata,
              P4EST_NUM_FIELD_HEADER_BYTES +
              1, "F %.13llu\n%-47s\n",
              (unsigned long long) quadrant_data->elem_size, user_string);

    /* write array-dependent metadata */
    mpiret =
      sc_io_write_at (fc->file, fc->accessed_bytes + write_offset,
                      array_metadata,
                      P4EST_NUM_FIELD_HEADER_BYTES, sc_MPI_BYTE, &count);

    P4EST_FILE_CHECK_MPI (mpiret, "Writing array metadata");
    count_error = (P4EST_NUM_FIELD_HEADER_BYTES != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (P4EST_NUM_FIELD_HEADER_BYTES, count);
  }
  P4EST_HANDLE_MPI_ERROR (mpiret, fc, fc->mpicomm, errcode);
  P4EST_HANDLE_MPI_COUNT_ERROR (count_error, fc, errcode);

  /* write array data */
  mpiret =
    sc_io_write_at_all (fc->file,
                        fc->accessed_bytes + write_offset +
                        P4EST_NUM_FIELD_HEADER_BYTES, quadrant_data->array,
                        bytes_to_write, sc_MPI_BYTE, &count);
  P4EST_FILE_CHECK_NULL (mpiret, fc, "Writing quadrant-wise", errcode);
  P4EST_FILE_CHECK_COUNT (bytes_to_write, count, fc, errcode);

  /** We place the padding bytes write here because for the sequential
   * IO operations the order of fwrite calls plays a role.
   */
  /* write padding bytes */
  if (rank == 0) {
    /* Calculate and write padding bytes for array data */
    array_size = fc->global_num_quadrants * quadrant_data->elem_size;
    get_padding_string (array_size, P4EST_BYTE_DIV, pad, &num_pad_bytes);

    mpiret =
      sc_io_write_at (fc->file,
                      fc->accessed_bytes + P4EST_NUM_METADATA_BYTES +
                      P4EST_BYTE_DIV + array_size +
                      P4EST_NUM_FIELD_HEADER_BYTES, pad, num_pad_bytes,
                      sc_MPI_BYTE, &count);
    P4EST_FILE_CHECK_MPI (mpiret, "Writing padding bytes for a data array");
    /* We do not need to call P4EST_FILE_HANDLE_MPI_ERROR in the next
     * collective line of code since P4EST_FILE_HANDLE_MPI_ERROR was already
     * called in this scope.
     */
    count_error = ((int) num_pad_bytes != count);
    P4EST_FILE_CHECK_COUNT_SERIAL (num_pad_bytes, count);
  }
  else {
    array_size = fc->global_num_quadrants * quadrant_data->elem_size;
    get_padding_string (array_size, P4EST_BYTE_DIV, NULL, &num_pad_bytes);
  }

  /* This is *not* the processor local value */
  fc->accessed_bytes +=
    quadrant_data->elem_size * fc->global_num_quadrants +
    P4EST_NUM_FIELD_HEADER_BYTES + num_pad_bytes;
  ++fc->num_calls;

  p4est_file_error_code (*errcode, errcode);
  return fc;
}

p4est_file_context_t *
p4est_file_read_field_ext (p4est_file_context_t * fc, p4est_gloidx_t * gfq,
                           sc_array_t * quadrant_data,
                           char user_string[P4EST_NUM_USER_STRING_BYTES],
                           int *errcode)
{
  int                 count;
  size_t              bytes_to_read, num_pad_bytes, array_size,
    read_data_size;
#ifdef P4EST_ENABLE_MPIIO
  sc_MPI_Offset       size;
#endif
  int                 mpiret, rank, mpisize;

  mpiret = sc_MPI_Comm_rank (fc->mpicomm, &rank);
  SC_CHECK_MPI (mpiret);
  mpiret = sc_MPI_Comm_size (fc->mpicomm, &mpisize);
  SC_CHECK_MPI (mpiret);

  P4EST_ASSERT (fc != NULL);
  P4EST_ASSERT (gfq != NULL);
  P4EST_ASSERT (errcode != NULL);

  /* check gfq in the debug mode */
  P4EST_ASSERT (gfq[0] == 0);
  P4EST_ASSERT (gfq[mpisize] == fc->global_num_quadrants);

  if (quadrant_data == NULL || quadrant_data->elem_size == 0) {
    /* Nothing to read but we shift our own file pointer */
    if (read_block_metadata (fc, &read_data_size, 0, 'F', NULL, errcode) ==
        NULL) {
      p4est_file_error_code (*errcode, errcode);
      return NULL;
    }

    /* calculate the padding bytes for this data array */
    array_size = fc->global_num_quadrants * read_data_size;
    get_padding_string (array_size, P4EST_BYTE_DIV, NULL, &num_pad_bytes);
    fc->accessed_bytes +=
      read_data_size * fc->global_num_quadrants +
      P4EST_NUM_FIELD_HEADER_BYTES + num_pad_bytes;
    ++fc->num_calls;
    *errcode = sc_MPI_SUCCESS;
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }

  sc_array_resize (quadrant_data, (size_t) (gfq[rank + 1] - gfq[rank]));

  /* check how many bytes we read from the disk */
  bytes_to_read = quadrant_data->elem_count * quadrant_data->elem_size;

#ifdef P4EST_ENABLE_MPIIO
  /* check file size; no sync required because the file size does not
   * change in the reading mode.
   */
  mpiret = MPI_File_get_size (fc->file, &size);
  P4EST_FILE_CHECK_NULL (mpiret, fc, "Get file size for read", errcode);
  if (((size_t) size) - P4EST_NUM_METADATA_BYTES - P4EST_BYTE_DIV -
      P4EST_NUM_FIELD_HEADER_BYTES < bytes_to_read) {
    /* report wrong file size, collectively close the file and deallocate fc */
    if (rank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_io: Error reading. File has less bytes than the user wants to read.\n");
    }
    mpiret = p4est_file_close (fc, &mpiret);
    P4EST_FILE_CHECK_NULL (mpiret, fc,
                           P4EST_STRING "_file_read_data: close file",
                           errcode);
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }
#else
  /* There is no C-standard functionality to get the file size */
#endif

  /* check the array metadata */
  if (read_block_metadata
      (fc, &read_data_size, quadrant_data->elem_size, 'F', user_string,
       errcode) == NULL) {
    p4est_file_error_code (*errcode, errcode);
    return NULL;
  }

  /* calculate the padding bytes for this data array */
  array_size = fc->global_num_quadrants * quadrant_data->elem_size;
  get_padding_string (array_size, P4EST_BYTE_DIV, NULL, &num_pad_bytes);

  mpiret = sc_io_read_at_all (fc->file,
                              fc->accessed_bytes +
                              P4EST_NUM_METADATA_BYTES +
                              P4EST_NUM_FIELD_HEADER_BYTES +
                              P4EST_BYTE_DIV + gfq[rank]
                              * quadrant_data->elem_size,
                              quadrant_data->array, bytes_to_read,
                              sc_MPI_BYTE, &count);

  P4EST_FILE_CHECK_NULL (mpiret, fc, "Reading quadrant-wise", errcode);
  P4EST_FILE_CHECK_COUNT (bytes_to_read, count, fc, errcode);

  fc->accessed_bytes +=
    quadrant_data->elem_size * fc->global_num_quadrants +
    P4EST_NUM_FIELD_HEADER_BYTES + num_pad_bytes;
  ++fc->num_calls;

  p4est_file_error_code (*errcode, errcode);
  return fc;
}

p4est_file_context_t *
p4est_file_read_field (p4est_file_context_t * fc, sc_array_t * quadrant_data,
                       char user_string[P4EST_NUM_USER_STRING_BYTES],
                       int *errcode)
{
  int                 mpiret, mpisize, rank;
  p4est_gloidx_t     *gfq = NULL;
  p4est_file_context_t *retfc;

  P4EST_ASSERT (fc != NULL);
  P4EST_ASSERT (errcode != NULL);

  /* If this function is used on a file context obtained by
   * \ref p4est_file_open_read the global_first_quadrant
   * array is set to the corresponding one of the p4est
   * given in the call \ref p4est_file_open_read.
   * Otherwise the file context was obtained by calling
   * \ref p4est_file_open_read_ext. This means the
   * global_first_quadrant array is not set since
   * there is no given p4est. In this case we
   * compute a uniform partition.
   */
  if (fc->global_first_quadrant == NULL) {
    /* there is no partition set in the file context */
    mpiret = sc_MPI_Comm_size (fc->mpicomm, &mpisize);
    SC_CHECK_MPI (mpiret);

    gfq = P4EST_ALLOC (p4est_gloidx_t, mpisize + 1);

    /* calculate gfq for a uniform partition */
    p4est_comm_global_first_quadrant (fc->global_num_quadrants, mpisize, gfq);
  }
  else {
    gfq = fc->global_first_quadrant;
  }

  mpiret = sc_MPI_Comm_rank (fc->mpicomm, &rank);
  SC_CHECK_MPI (mpiret);

  if (quadrant_data != NULL) {
    /* allocate the memory for the qudrant data */
    sc_array_resize (quadrant_data, (size_t) (gfq[rank + 1] - gfq[rank]));
  }

  retfc = p4est_file_read_field_ext (fc, gfq, quadrant_data,
                                     user_string, errcode);
  if (fc->global_first_quadrant == NULL) {
    P4EST_FREE (gfq);
  }

  p4est_file_error_code (*errcode, errcode);
  return retfc;
}

int
p4est_file_info (p4est_t * p4est, const char *filename,
                 char user_string[P4EST_NUM_USER_STRING_BYTES],
                 sc_array_t * data_sections, int *errcode)
{
  int                 mpiret, eclass;
  int                 retval;
  int                 count, count_error;
  long                long_header;
  size_t              current_size, num_pad_bytes;
  char                metadata[P4EST_NUM_METADATA_BYTES + 1];
  char                block_metadata[P4EST_NUM_FIELD_HEADER_BYTES + 1];
  p4est_gloidx_t      global_num_quadrants;
  p4est_file_section_metadata_t *current_member;
  sc_MPI_Offset       current_position;
  sc_MPI_File         file;

  P4EST_ASSERT (p4est != NULL);
  P4EST_ASSERT (p4est_is_valid (p4est));
  P4EST_ASSERT (filename != NULL);
  P4EST_ASSERT (user_string != NULL);
  P4EST_ASSERT (data_sections != NULL);
  P4EST_ASSERT (data_sections->elem_size ==
                sizeof (p4est_file_section_metadata_t));
  P4EST_ASSERT (errcode != NULL);

  /* set default output values */
  sc_array_reset (data_sections);

  /* open the file in reading mode */
  *errcode = eclass = sc_MPI_SUCCESS;   /* MPI defines MPI_SUCCESS to equal 0. */
  file = sc_MPI_FILE_NULL;

  if ((retval =
       sc_io_open (p4est->mpicomm, filename, SC_READ,
                   sc_MPI_INFO_NULL, &file)) != sc_MPI_SUCCESS) {
    mpiret = sc_io_error_class (retval, &eclass);
    SC_CHECK_MPI (mpiret);
  }

  if (eclass) {
    *errcode = eclass;
    SC_FREE (file);
    p4est_file_error_code (*errcode, errcode);
    return -1;
  }

  /* read file metadata on root rank */
  P4EST_ASSERT (!eclass);
  if (p4est->mpirank == 0) {
    if ((retval = sc_io_read_at (file, 0, metadata,
                                 P4EST_NUM_METADATA_BYTES, sc_MPI_BYTE,
                                 &count))
        != sc_MPI_SUCCESS) {
      mpiret = sc_io_error_class (retval, &eclass);
      SC_CHECK_MPI (mpiret);
      *errcode = eclass;
      /* There is no count error for a non-successful read. */
      count_error = 0;
    }
    else {
      count_error = (P4EST_NUM_METADATA_BYTES != count);
    }
  }
  mpiret = sc_MPI_Bcast (&eclass, 1, sc_MPI_INT, 0, p4est->mpicomm);
  SC_CHECK_MPI (mpiret);
  if (eclass) {
    p4est_file_error_cleanup (&file);
    p4est_file_error_code (*errcode, errcode);
    return -1;
  }
  mpiret = sc_MPI_Bcast (&count_error, 1, sc_MPI_INT, 0, p4est->mpicomm);
  SC_CHECK_MPI (mpiret);
  if (count_error) {
    if (p4est->mpirank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_file_info: read count error for file metadata reading");
    }
    *errcode = P4EST_FILE_COUNT_ERROR;
    p4est_file_error_cleanup (&file);
    p4est_file_error_code (*errcode, errcode);
    return -1;
  }

  /* broadcast file metadata to all ranks and null-terminate it */
  mpiret = sc_MPI_Bcast (metadata, P4EST_NUM_METADATA_BYTES, sc_MPI_BYTE, 0,
                         p4est->mpicomm);
  SC_CHECK_MPI (mpiret);
  metadata[P4EST_NUM_METADATA_BYTES] = '\0';

  if ((mpiret =
       check_file_metadata (p4est->mpicomm, filename, user_string,
                            metadata,
                            &global_num_quadrants)) != sc_MPI_SUCCESS) {
    *errcode = P4EST_ERR_IO;
    p4est_file_error_code (*errcode, errcode);
    return p4est_file_error_cleanup (&file);
  }

  /* check global number of quadrants */
  if (p4est->global_num_quadrants != global_num_quadrants) {
    if (p4est->mpirank == 0) {
      P4EST_LERROR (P4EST_STRING
                    "_file_info: global number of quadrant mismatch");
    }
    *errcode = P4EST_ERR_IO;
    p4est_file_error_code (*errcode, errcode);
    return p4est_file_error_cleanup (&file);
  }

  current_position =
    (sc_MPI_Offset) (P4EST_NUM_METADATA_BYTES + P4EST_BYTE_DIV);

  /* read all data headers that we find and skip the data itself */
  if (p4est->mpirank == 0) {
    for (;;) {
      /* read block metadata for current record */
      mpiret = sc_io_read_at (file, current_position, block_metadata,
                              P4EST_NUM_FIELD_HEADER_BYTES, sc_MPI_BYTE,
                              &count);
      mpiret = sc_io_error_class (mpiret, &eclass);
      SC_CHECK_MPI (mpiret);
      *errcode = eclass;
      if (eclass) {
        p4est_file_error_code (*errcode, errcode);
        return p4est_file_error_cleanup (&file);
      }
      if (P4EST_NUM_FIELD_HEADER_BYTES != count) {
        /* we did not read the correct number of bytes */
        break;
      }

      /* parse and store the element size, the block type and the user string */
      current_member =
        (p4est_file_section_metadata_t *) sc_array_push (data_sections);
      if (block_metadata[0] == 'H' || block_metadata[0] == 'F') {
        /* we want to read the block type */
        current_member->block_type = block_metadata[0];
      }
      else {
        /* the last entry is incomplete and is therefore removed */
        sc_array_rewind (data_sections, data_sections->elem_count - 1);
        /* current_member is freed if the whole array is freed */
        break;
      }

      /* check format */
      if (block_metadata[P4EST_NUM_ARRAY_METADATA_BYTES + 1] != '\n') {
        /* the last entry is incomplete and is therefore removed */
        sc_array_rewind (data_sections, data_sections->elem_count - 1);
        break;
      }

      /* process block metadata */
      block_metadata[P4EST_NUM_ARRAY_METADATA_BYTES + 1] = '\0';
      /* we cut off the block type specifier to read the data size */
      current_member->data_size = (size_t) sc_atol (&block_metadata[2]);

      /* read the user string */
      /* check '\n' to check the format */
      if (block_metadata[P4EST_NUM_FIELD_HEADER_BYTES - 1] != '\n') {
        /* the last entry is incomplete and is therefore removed */
        sc_array_rewind (data_sections, data_sections->elem_count - 1);
        break;
      }

      /* null-terminate the user string of the current block */
      block_metadata[P4EST_NUM_FIELD_HEADER_BYTES - 1] = '\0';

      /* copy the user string, '\0' was already set above */
      strncpy (current_member->user_string,
               &block_metadata[P4EST_NUM_ARRAY_METADATA_BYTES + 2],
               P4EST_NUM_USER_STRING_BYTES);
      P4EST_ASSERT (current_member->user_string
                    [P4EST_NUM_USER_STRING_BYTES - 1] == '\0');

      /* get padding bytes of the current block */
      if (current_member->block_type == 'F') {
        current_size =
          (size_t) (p4est->global_num_quadrants * current_member->data_size);
      }
      else if (current_member->block_type == 'H') {
        current_size = current_member->data_size;
      }
      else {
        /* \ref read_block_metadata checks for valid block type */
        SC_ABORT_NOT_REACHED ();
      }
      get_padding_string (current_size, P4EST_BYTE_DIV, NULL, &num_pad_bytes);
      /* read padding bytes */
      mpiret = sc_io_read_at (file,
                              current_position +
                              P4EST_NUM_FIELD_HEADER_BYTES + current_size,
                              block_metadata, num_pad_bytes, sc_MPI_BYTE,
                              &count);
      mpiret = sc_io_error_class (mpiret, &eclass);
      SC_CHECK_MPI (mpiret);
      *errcode = eclass;
      if (eclass) {
        return p4est_file_error_cleanup (&file);
      }
      /* check '\n' in padding bytes */
      if (block_metadata[0] != '\n'
          || block_metadata[num_pad_bytes - 1] != '\n') {
        /* the last entry is incomplete and is therefore removed */
        P4EST_LERROR (P4EST_STRING
                      "_file_info: stop parsing file and discard last element "
                      "due to wrong padding format.\n");
        sc_array_rewind (data_sections, data_sections->elem_count - 1);
        break;
      }
      current_position +=
        P4EST_NUM_FIELD_HEADER_BYTES + current_size + num_pad_bytes;
    }
  }

  /* replicate block metadata in parallel */
  long_header = (long) data_sections->elem_count;       /* 0 on non-root */
  mpiret = sc_MPI_Bcast (&long_header, 1, sc_MPI_LONG, 0, p4est->mpicomm);
  SC_CHECK_MPI (mpiret);
  if (p4est->mpirank != 0) {
    sc_array_resize (data_sections, (size_t) long_header);
  }
  mpiret = sc_MPI_Bcast (data_sections->array,
                         data_sections->elem_count * data_sections->elem_size,
                         sc_MPI_BYTE, 0, p4est->mpicomm);
  SC_CHECK_MPI (mpiret);

  /* close the file with error checking */
  P4EST_ASSERT (!eclass);
#ifdef P4EST_ENABLE_MPIIO
  if ((retval = sc_MPI_File_close (&file)) != sc_MPI_SUCCESS) {
    mpiret = sc_io_error_class (retval, &eclass);
    *errcode = eclass;
    SC_CHECK_MPI (mpiret);
  }
#else
  if (p4est->mpirank == 0) {
    errno = 0;
    if (fclose (file->file)) {
      mpiret = sc_io_error_class (errno, &eclass);
      SC_CHECK_MPI (mpiret);
      *errcode = eclass;
    }
  }
  else {
    P4EST_ASSERT (file->file == NULL);
  }
  SC_FREE (file);
  mpiret = sc_MPI_Bcast (&eclass, 1, sc_MPI_INT, 0, p4est->mpicomm);
  SC_CHECK_MPI (mpiret);
#endif
  p4est_file_error_code (*errcode, errcode);
  return 0;
}

/** Converts a error code (MPI or errno) into a p4est_file error code.
 * This function turns MPI error codes into MPI error classes if
 * MPI IO is enabled.
 * If MPI IO is not enabled, the function processes the errors outside
 * of MPI but passes version 1.1 errors to MPI_Error_class.
 * Furthermore, p4est_file functions can create \ref P4EST_FILE_COUNT_ERROR
 * as errcode what is also processed by this function.
 * \param [in]  errcode     An errcode from a p4est_file function.
 * \param [out] p4est_errcode Non-NULL pointer. Filled with matching
 *                          error code on success.
 * \return                  sc_MPI_SUCCESS on successful conversion.
 *                          Other MPI error code otherwise.
 */
static int
p4est_file_error_code (int errcode, int *p4est_errcode)
{
  P4EST_ASSERT (p4est_errcode != NULL);

  /* count error exists only on p4est-level */
  if (errcode == P4EST_FILE_COUNT_ERROR) {
    *p4est_errcode = errcode;
    return sc_MPI_SUCCESS;
  }
  else {
    /* all other errcodes can be handled by libsc */
    return sc_io_error_class (errcode, p4est_errcode);

  }
}

int
p4est_file_error_string (int errclass, char string[sc_MPI_MAX_ERROR_STRING],
                         int *resultlen)
{
  int                 retval;

  P4EST_ASSERT (resultlen != NULL);

  if (string == NULL || resultlen == NULL) {
    return sc_MPI_ERR_ARG;
  }

  /* count error exists only on p4est-level */
  if (errclass == P4EST_FILE_COUNT_ERROR) {
    if ((retval =
         snprintf (string, sc_MPI_MAX_ERROR_STRING, "%s",
                   "Read or write count error")) < 0) {
      /* unless something goes against the current standard of snprintf */
      return sc_MPI_ERR_NO_MEM;
    }
    if (retval >= sc_MPI_MAX_ERROR_STRING) {
      retval = sc_MPI_MAX_ERROR_STRING - 1;
    }
    *resultlen = retval;
    return sc_MPI_SUCCESS;
  }
  else {
    /* all other errocodes can be handled by libsc */
    return sc_MPI_Error_string (errclass, string, resultlen);
  }
}

int
p4est_file_close (p4est_file_context_t * fc, int *errcode)
{
  P4EST_ASSERT (fc != NULL);
  P4EST_ASSERT (errcode != NULL);

  int                 mpiret;

  mpiret = sc_io_close (&fc->file);
  P4EST_FILE_CHECK_INT (mpiret, "Close file", errcode);

  if (fc->gfq_owned) {
    P4EST_FREE (fc->global_first_quadrant);
  }
  P4EST_FREE (fc);

  p4est_file_error_code (*errcode, errcode);
  return 0;
}
