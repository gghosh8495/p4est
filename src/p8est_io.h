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

/** \file p8est_io.h
 *
 * Provide functions to serialize/deserialize a forest.
 * Some are used as building blocks for \ref p8est_load and \ref p8est_save.
 * Others allow for saving and loading user-defined data to a parallel file.
 *
 * Furthermore, this module provides functions to write and read general data
 * files associated with a p8est.
 */

#ifndef P8EST_IO_H
#define P8EST_IO_H

#include <p8est.h>

SC_EXTERN_C_BEGIN;

#define P8EST_DATA_FILE_EXT "p8data" /**< file extension of p8est data files */
#define P8EST_MAGIC_NUMBER "p8data0" /**< magic string for p8est data files */
#define P8EST_NUM_METADATA_BYTES 64 /**< number of file metadata bytes */
#define P8EST_NUM_ARRAY_METADATA_BYTES 14 /**< number of array metadata bytes */
/* subtract 2 for '\n' at the beginning and end of the array metadata */
#define P8EST_NUM_ARRAY_METADATA_CHARS (P8EST_NUM_ARRAY_METADATA_BYTES - 2) /**< number of array metadata chars */
#define P8EST_BYTE_DIV 16 /**< All data blocks are padded to be divisible by this. */
#define P8EST_MAX_NUM_PAD_BYTES (P4EST_BYTE_DIV + 1) /**< We enforce to pad in any
                                                          case and the padding string
                                                          needs to contain two
                                                          newline characters and
                                                          therefore this is the
                                                          maximal number of pad
                                                          bytes. */
#define P8EST_NUM_USER_STRING_BYTES 48 /**< number of user string bytes */
#define P8EST_NUM_FIELD_HEADER_BYTES (2 + P8EST_NUM_ARRAY_METADATA_BYTES + P8EST_NUM_USER_STRING_BYTES)
                                     /**< number of bytes of one field header */
#define P8EST_FILE_COUNT_ERROR -1 /**< All other error codes are defined by MPI or are
                                     errno. This error code is used to indicate a read
                                     or write count error that may be occurred during a
                                     MPI IO operation or a IO operation called by C
                                     standard functions. */

/** This macro is used for file format errors. */
#ifndef P4EST_ENABLE_MPIIO
#define P8EST_ERR_IO EIO /**< File format error code without MPI IO */
#else
#define P8EST_ERR_IO sc_MPI_ERR_IO /**< File format error with MPI IO */
#endif

/** Extract processor local quadrants' x y z level data.
 * Optionally extracts the quadrant data as well into a separate array.
 * \param [in] p8est    The forest is not modified.
 * \param [in,out] data If not NULL, pointer to a pointer that will be set
 *                      to a newly allocated array with per-quadrant data.
 *                      Must be NULL if p4est->data_size == 0.
 * \return              An array of type p8est_qcoord_t that contains
 *                      x y z level for each quadrant on this processor.
 *                      The tree information is not extracted.
 */
sc_array_t         *p8est_deflate_quadrants (p8est_t * p8est,
                                             sc_array_t ** data);

/** Create a new p4est based on serialized data.
 * Its revision counter is set to zero.
 * See p8est.h and p8est_communication.h for more information on parameters.
 * \param [in] mpicomm       A valid MPI communicator.
 * \param [in] connectivity  This is the connectivity information that
 *                           the forest is built with.  Note that p4est
 *                           does not take ownership of the memory.
 * \param [in] global_first_quadrant First global quadrant on each proc and
 *                           one beyond.  Copied into global_first_quadrant.
 *                           Local count on rank is gfq[rank + 1] - gfq[rank].
 * \param [in] pertree       The cumulative quadrant counts per tree.
 * \param [in] quadrants     Array as returned by p8est_deflate_quadrants.
 * \param [in] data          Array as from p8est_deflate_quadrants or NULL.
 *                           The elem_size of this array informs data_size.
 *                           Its elem_count equals the number of local quads.
 * \param [in] user_pointer  Assign to the user_pointer member of the p4est.
 * \return              The newly created p4est with a zero revision counter.
 */
p8est_t            *p8est_inflate (sc_MPI_Comm mpicomm,
                                   p8est_connectivity_t * connectivity,
                                   const p4est_gloidx_t *
                                   global_first_quadrant,
                                   const p4est_gloidx_t * pertree,
                                   sc_array_t * quadrants, sc_array_t * data,
                                   void *user_pointer);

/** p8est data file format
 * All p4est data files have 64 bytes file header at the beginning of the file.
 * The file header is written to the file as string without null-termination
 * (called string*) and is therefore readable in a text editor.
 *
 * File Header (64 bytes):
 * 7 bytes magic number (p8data0) and 1 byte new line char.
 * 23 bytes p4est version string* and 1 byte new line char.
 * 15 bytes user string*  and 1 byte new line char.
 * 16 bytes number of global quadrants.
 *
 * The file header is padded by 16 bytes consisting of 1 byte
 * new line char succeeded by 14 bytes of spaces and 1 trailing byte
 * new line char.
 *
 * The actual data is stored in arrays corresponding to a mesh of a p4est
 * or in header blocks that have a fixed user-defined size. The header
 * blocks are written and read on rank 0.
 * One data array stores a fixed number of bytes of user-
 * defined data per quadrant of a certain p4est. Therefore, one user-defined
 * data array is of the size p4est->global_num_quadrants * data_size, where
 * data_size is set by the user. The file format is partition independent.
 * The data arrays are padded such that the number of bytes for
 * an array is divisible by 16. The padding also enforced for data blocks
 * that have a size that is divisble by 16.
 * The p4est data file consists of a variable number (including 0) of
 * these two types of blocks.
 * Every data block is preceded by 64 bytes block header written
 * by p4est. These 64 bytes are again written to the file as string* and can
 * be read using a text editor.
 *
 * Block Header (64 bytes):
 * One byte block type specific character (H for a header block and F for
 * a data array), 1 byte space and 13 bytes size in number of bytes for a
 * header block and data size per element in byte for a data array block
 * and one trailing byte new line char.
 * 47 bytes user-defined string* and 1 byte new line char.
 *
 * The structure of p4est and p8est data files differs only by the magic number.
 *
 * The p4est metadata of a p4est data file can be accessed by \ref p8est_file_info().
 */

/** Opaque context used for writing a p8est data file. */
typedef struct p8est_file_context p8est_file_context_t;

/** Begin writing file header and saving data blocks into a parallel file.
 *
 * This function creates a new file or overwrites an existing one.
 * It is collective and creates the file on a parallel file system.
 * It takes an (optional) pointer to write a header of given size.
 * This function leaves the file open if MPI I/O is available.
 * It is necessary to call \ref
 * p8est_file_close (possibly after writing one or more data sets).
 * The file is opened in a write-only mode.
 *
 * We add some basic metadata to the file.
 * The file written contains the file header and data sets
 * as specified by the open/write functions called.
 * The file header consists of the metadata specified by p4est.
 *
 * It is the application's responsibility to write sufficient header
 * information (cf. \ref p8est_file_write_header) to determine the number and
 * size of the data sets if such information is not recorded and maintained
 * externally.
 * However, p4est makes some metadata accessible via
 * \ref p8est_file_info.
 *
 * This function does not abort on MPI I/O errors but returns NULL.
 *
 * \param [in] p8est          Valid forest.
 * \param [in] filename       Path to parallel file that is to be created.
 * \param [in] user_string    A user string that is written to the file header.
 *                            Only 15 bytes without null-termination are
 *                            written to the file. If the user gives less
 *                            bytes the user_string in the file header is padded
 *                            by spaces.
 * \param [out] errcode       An errcode that can be interpreted by \ref
 *                            p8est_file_error_string and
 *                            \ref p8est_file_error_class.
 * \return                    Newly allocated context to continue writing
 *                            and eventually closing the file. NULL in
 *                            case of error.
 */
p8est_file_context_t *p8est_file_open_create
  (p8est_t * p8est, const char *filename, char user_string[15], int *errcode);

/** Open a file for reading and read its user string on rank zero.
 * The user string is broadcasted to all ranks after reading.
 * The file must exist and be at least of the size of the file header.
 *
 * If the file has wrong metadata the function reports the error using
 * /ref P8EST_LERRORF, collectively close the file and deallocate
 * the file context. In this case the function returns NULL on all ranks.
 * The wrong file format or a wrong file header causes \ref P8EST_ERR_IO
 * as errcode.  
 *
 * This function does not abort on MPI I/O errors but returns NULL.
 *
 * \param [in] p8est            The forest must be of the same refinement
 *                              pattern as the one used for writing the file.
 *                              Its global number of quadrants must match.
 *                              It is possible, however, to use a different
 *                              partition or number of ranks from writing it.
 * \param [in] filename         The path to the file that is opened.
 * \param [in,out] user_string  At least 16 bytes. The user string is written
 *                              to the passed array including padding spaces
 *                              and a trailing null-termination.
 * \param [out] errcode         An errcode that can be interpreted by \ref
 *                              p8est_file_error_string and
 *                              \ref p8est_file_error_class.
 * \return                      Newly allocated context to continue reading
 *                              and eventually closing the file. NULL in
 *                              case of error.
 */
p8est_file_context_t *p8est_file_open_read (p8est_t * p8est,
                                            const char *filename,
                                            char *user_string, int *errcode);

/** Write a header block to an opened file.
 * This function requires an opened file context.
 * The header data and its metadata are written on rank 0.
 *
 * \param [out] fc            Context previously created by \ref
 *                            p8est_file_open_create.
 * \param [in]  header_size   The size of header_data in bytes.
 *                            This function returns the passed fc
 *                            parameter and sets errcode to
 *                            sc_MPI_SUCCESS if it is called
 *                            for header_size == 0. Nothing is
 *                            written to the file and fc stays
 *                            untouched.
 * \param [in]  header_data   A pointer to the header data. The user is
 *                            responsible for the validality of the header
 *                            data.
 * \param [in]  user_string   Maximal 47 bytes. These chars are written to
 *                            the block header and padded to 47 chars by adding
 *                            spaces.
 * \param [out] errcode       An errcode that can be interpreted by \ref
 *                            p8est_file_error_string and
 *                            \ref p8est_file_error_class.
 * \return                    Return the input context to continue writing
 *                            and eventually closing the file. The return
 *                            value is NULL in case of error, then
 *                            it also holds errcode != 0 and the file is
 *                            tried to close and fc is freed.
 */
p8est_file_context_t *p8est_file_write_header (p8est_file_context_t * fc,
                                               size_t header_size,
                                               const void *header_data,
                                               char user_string[47],
                                               int *errcode);

/** Read a header block from an opened file.
 * This function requires an opened file context.
 * The header data is read on rank 0.
 *
 * If the user does not have the header_size to call this function, the user
 * can user \ref p8est_file_info to obtain the required information.
 *
 * The passed header_size is compared to the header_size stored in the file.
 * If the values do not equal each other, the function reports details via
 * /ref P8EST_LERRORF and closes and deallocate the file context. The return
 * value in this case is NULL.
 * If the block header information is not matching the passed parameters
 * the function sets \ref P8EST_ERR_IO for errcode.
 *
 * \param [out] fc              Context previously created by \ref
 *                              p8est_file_open_create.
 * \param [in]  header_size     The size of the header that is read.
 * \param [in, out] header_data header_size allocated bytes. This data will be
 *                              filled with the header data from file. If this
 *                              is NULL it means that the current header block
 *                              is skipped and the internal file pointer of the
 *                              file context is set to the next data block. If
 *                              current data block is not a header block, the
 *                              file is closed and the file context is
 *                              deallocated. Furthermore, in this case the
 *                              function returns NULL and sets errcode to
 *                              \ref P8EST_ERR_IO.
 * \param [in,out] user_string  At least 48 bytes. Filled by the padded user
 *                              string and a trailing null-termination char.
 * \param [out] errcode         An errcode that can be interpreted by \ref
 *                              p8est_file_error_string and
 *                              \ref p8est_file_error_class.
 * \return                      Return the input context to continue reading
 *                              and eventually closing the file. The return value
 *                              is NULL if the function was called for
 *                              header_size == 0. The return
 *                              value is also NULL in case of error but then
 *                              it also holds errcode != 0 and the file is
 *                              tried to close and fc is freed.
 */
p8est_file_context_t *p8est_file_read_header (p8est_file_context_t * fc,
                                              size_t header_size,
                                              void *header_data,
                                              char *user_string,
                                              int *errcode);

/** Write one (more) per-quadrant data set to a parallel output file.
 *
 * This function requires an opened file context.
 * The data set is appended to the header/previously written data sets.
 * This function writes a block of the size number of quadrants * data_size.
 *
 * This function does not abort on MPI I/O errors but returns NULL.
 *
 * \param [out] fc            Context previously created by \ref
 *                            p8est_file_open_create or \ref
 *                            p8est_file_open_append.
 * \param [in] quadrant_data  An array of the length number of local quadrants
 *                            with the element size equal to number of bytes
 *                            written per quadrant. The quadrant data is expected
 *                            to be stored according to the Morton order of
 *                            the quadrants. For quadrant_data->elem_size == 0
 *                            the function does nothing and returns the unchanged
 *                            file context. In this case errcode is set
 *                            to sc_MPI_SUCCESS.
 * \param [in] user_string    An array of maximal 47 bytes that is written
 *                            after the array-dependent metadata and before
 *                            the actual data. If the array is shorter the
 *                            written char array will be padded to the
 *                            right by spaces. The user_string is
 *                            written on rank 0 and therefore also only
 *                            required on rank 0. Can be NULL for other
 *                            ranks.
 * \param [out] errcode       An errcode that can be interpreted by \ref
 *                            p8est_file_error_string and
 *                            \ref p8est_file_error_class.
 * \return                    Return the input context to continue writing
 *                            and eventually closing the file. The return value
 *                            is NULL if the function was called for
 *                            quadrant_data->elem_size == 0. The return
 *                            value is also NULL in case of error but then
 *                            it also holds errcode != 0 and the file is
 *                            tried to close and fc is freed.
 */
p8est_file_context_t *p8est_file_write_field (p8est_file_context_t * fc,
                                              sc_array_t * quadrant_data,
                                              char user_string[47],
                                              int *errcode);

/** Read one (more) per-quadrant data set from a parallel input file.
 * This function requires the appropriate number of readable bytes.
 * In practice, the data size to read should match the size written.
 * This function aborts if the number of bytes to read is bigger than the
 * dataset that corresponds to the processor.
 * The data size to read is encoded by the element size of quadrant_data
 * It is possible to skip over a data set to read by a NULL \ref sc_array.
 * It is legal to close a file before all data sets have been read.
 *
 * The function closes and deallocates the file context and returns NULL
 * if the bytes the user wants to read exceed the given file and/or
 * the element size of the array given by quadrant_data->elem_size does not
 * coincide with the element size according to the array metadata given in
 * the file.
 *
 * If the block header information is not matching the passed parameters
 * the function sets \ref P8EST_ERR_IO for errcode.
 * 
 * This function does not abort on MPI I/O errors but returns NULL.
 *
 * \param [in,out] fc         Context previously created by \ref
 *                            p8est_file_open_read(_ext).  It keeps track
 *                            of the data sets read one after another.
 * \param [in,out] quadrant_data  An array of the length number of local quadrants
 *                            with the element size equal to number of bytes
 *                            read per quadrant. The quadrant data is read
 *                            according to the Morton order of the quadrants.
 *                            For quadrant_data->elem_size == 0
 *                            the function does nothing and returns the unchanged
 *                            file context. For quadrant_data == NULL the
 *                            function skips one data array in the file.
 *                            If fc was opened by \ref p8est_file_open_read_ext
 *                            and fc->global_first_quadrant was not set by the
 *                            user, the function uses a uniform partition to read
 *                            the data field in parallel.
 *                            quadrant_data is resized by \ref sc_array_resize.
 * \param [in,out]  user_string At least 48 bytes. The user string
 *                            is read on rank 0 and internally broadcasted
 *                            to all ranks.
 * \param [out] errcode       An errcode that can be interpreted by \ref
 *                            p8est_file_error_string and
 *                            \ref p8est_file_error_class.
 * \return                    Return a pointer to input context or NULL in case
 *                            of errors that does not abort the program or if
 *                            the function was called with quadrant_data == NULL.
 *                            In case of error the file is tried to close
 *                            and fc is freed.
 */
p8est_file_context_t *p8est_file_read_field (p8est_file_context_t * fc,
                                             sc_array_t * quadrant_data,
                                             char *user_string, int *errcode);

/** A data type that encodes the metadata of one data block in a p4est data file.
 */
typedef struct p8est_file_block_metadata
{
  char                block_type; /**< 'H' (header) or 'F' (data file) */
  size_t              data_size;  /**< data size in bytes per array element ('F')
                                       or of the header block ('H') */
  char                user_string[48]; /**< user string of the data block */
}
p8est_file_block_metadata_t;

/** Read metadata information of a file written by a matching forest.
 * Matching refers to the global count of quadrants; partition is irrelevant.
 *
 * This function parses the given file on rank 0 and broadcasts the information
 * on the number of data fields contained to all other ranks.  Collective call.
 *
 * This function catches all I/O and file format errors and returns a valid MPI
 * error class related to file handling.  Errors are collectively synchronized.
 *
 * If the number of bytes that the user intend to read is larger than the number
 * bytes left in the file, the function prints out an information about this
 * situation using \ref P8EST_LERROR. In this case the function reads the bytes
 * that are possible to read but returns NULL to indicate an error.
 * If the file or block header information is not matching the passed parameters
 * the function sets \ref P8EST_ERR_IO for errcode.
 *
 * \param [in]  p4est               A p4est that is only required for the
 *                                  MPI communicator, and to verify the
 *                                  global quadrant count found in the file.
 * \param [in]  filename            Path to parallel file.
 * \param [in,out] user_string      At least 16 bytes. This array will
 *                                  be filled with the user string of the
 *                                  file after a successful call of this
 *                                  function.
 * \param [in,out] blocks           After a successful function call this
 *                                  variable holds an array with a length
 *                                  corresponding to the number of arrays in the
 *                                  file that are successfully found and seeked.
 *                                  The values in the array are the
 *                                  number of bytes of stored data per quadrant.
 *                                  Require elem_size->elem_size
 *                                  == sizeof (p8est_file_block_metadata_t)
 *                                  on input and preserve it on output.
 *                                  See p4est_file_block_metadata_t to obtain
 *                                  detailed information about the data blocks
 *                                  of the file.
 * \param [out] errcode             An errcode that can be interpreted by \ref
 *                                  p4est_file_error_string and
 *                                  \ref p8est_file_error_class.
 * \return                          0 for a successful call and -1 in case of
 *                                  an error. See also \ref errcode argument.
 */
int                 p8est_file_info (p8est_t * p8est, const char *filename,
                                     char *user_string,
                                     sc_array_t * blocks, int *errcode);

/** Converts a p8est file error code into a p8est_file error class.
 * This function turns MPI error codes into MPI error classes if
 * MPI IO is enabled.
 * If MPI IO is not enabled, the function processes the errors outside
 * of MPI but passes version 1.1 errors to MPI_Error_class.
 * Furthermore, p8est_file functions can create \ref P8EST_FILE_COUNT_ERROR
 * as errcode what is also processed by this function.
 * \param [in]  errcode     An errcode from a p8est_file function.
 * \param [out] errclass    Non-NULL pointer. Filled with matching
 *                          errclass on success.
 * \return                  sc_MPI_SUCCESS on successful conversion.
 *                          Other MPI error code otherwise.
 */
int                 p8est_file_error_class (int errcode, int *errclass);

/** Turn p8est_file errcode into a string.
 * errclass must be a class that is output by \ref p8est_file_error_class.
 * \param [in] errclass     An errclass that is output by \ref
 *                          p8est_file_error_class.
 * \param [in,out] string   At least sc_MPI_MAX_ERROR_STRING bytes.
 * \param [out] resultlen   Length of string on return.
 * \return                   sc_MPI_SUCCESS on success or
 *                           something else on invalid arguments.
 */
int                 p8est_file_error_string (int errclass, char *string,
                                             int *resultlen);

/** Close a file opened for parallel write/read and free the context.
 * \param [in,out] fc       Context previously created by \ref
 *                          p8est_file_open_create or \ref
 *                          p8est_file_open_read_(ext).  Is freed.
 * \param [out] errcode     An errcode that can be interpreted by \ref
 *                          p8est_file_error_string and
 *                          \ref p8est_file_error_class.
 * \return                  0 for a successful call and -1 in case of
 *                          an error. See also \ref errcode argument.
 */
int                 p8est_file_close (p8est_file_context_t * fc,
                                      int *errcode);

SC_EXTERN_C_END;

#endif /* !P8EST_IO_H */
