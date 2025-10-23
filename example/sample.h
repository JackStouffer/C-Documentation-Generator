/**
 * @file sample.h
 * @brief Example declarations used to showcase the documentation generator.
 *
 * This header defines a handful of macros, typedefs, and functions that exercise
 * the generator's ability to understand docstrings, parameter lists, and code
 * blocks. Nothing here is production ready — it simply demonstrates formatting
 * and annotation styles the tool can consume.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Major version of the sample API.
 */
#define SAMPLE_VERSION_MAJOR 1

/**
 * Minor version of the sample API.
 */
#define SAMPLE_VERSION_MINOR 0

/**
 * Expands to the total number of elements in a static array.
 *
 * @param arr Static array whose element count should be computed.
 */
#define SAMPLE_ARRAY_LENGTH(arr) (sizeof(arr) / sizeof((arr)[0]))

/**
 * Wraps a statement with a retry loop that exits after `max_attempts`.
 *
 * @param expr Statement that returns `true` on success.
 * @param max_attempts Maximum number of iterations before giving up.
 *
 * Example usage:
 * @code{.c}
 * SAMPLE_RETRY(sample_connect(), 3) {
 *     sample_sleep(10);
 * }
 * @endcode
 */
#define SAMPLE_RETRY(expr, max_attempts)                                         \
    for (unsigned _sample_try = 0; _sample_try < (unsigned)(max_attempts); ++_sample_try) \
        if (expr)                                                                \
            break;                                                               \
        else

/**
 * Enumerates the possible results returned by sample operations.
 */
typedef enum sample_result {
    SAMPLE_OK = 0,
    SAMPLE_ERR_INVALID_ARGUMENT,
    SAMPLE_ERR_IO,
    SAMPLE_ERR_TIMEOUT
} sample_result_t;

/**
 * Describes how the processor should interpret incoming data.
 */
typedef enum sample_mode {
    SAMPLE_MODE_BINARY,
    SAMPLE_MODE_TEXT,
    SAMPLE_MODE_COMPRESSED
} sample_mode_t;

/**
 * Opaque handle that callers use to stream bytes into the processing pipeline.
 */
typedef struct SampleBuffer {
    const uint8_t *data;
    size_t length;
    size_t position;
} SampleBuffer;

/**
 * Configuration structure passed to `sample_run`.
 *
 * @note Call `sample_init_config` to populate sensible defaults before making
 *       manual adjustments.
 */
typedef struct SampleConfig {
    sample_mode_t mode;
    bool enable_logging;
    uint32_t max_batch_size;
    const char *log_path;
} SampleConfig;

/**
 * Function pointer invoked whenever the processor emits a log message.
 *
 * @param level Severity string such as `"info"` or `"error"`.
 * @param message Null-terminated log payload.
 * @param user_data Custom pointer specified during `sample_set_logger`.
 */
typedef void (*sample_logger_fn)(const char *level, const char *message, void *user_data);

/**
 * Initializes `config` with safe defaults.
 *
 * @param config Structure to initialize. Must not be `NULL`.
 */
void sample_init_config(SampleConfig *config);

/**
 * Registers a callback to receive log messages from the sample processor.
 *
 * @param logger Function invoked for each emitted log line. Pass `NULL` to disable logging.
 * @param user_data Pointer forwarded to every invocation of `logger`.
 */
void sample_set_logger(sample_logger_fn logger, void *user_data);

/**
 * Processes a block of data using the provided configuration.
 *
 * @param config Active configuration generated via `sample_init_config`.
 * @param input Pointer to the bytes to process. The buffer is not owned.
 * @param length Number of bytes available at `input`.
 * @return A `sample_result_t` describing success or failure.
 *
 * @note The function returns immediately when `length` is zero.
 * @warning Passing a `NULL` pointer while `length` is non-zero triggers `SAMPLE_ERR_INVALID_ARGUMENT`.
 */
sample_result_t sample_run(const SampleConfig *config, const uint8_t *input, size_t length);

/**
 * Reads a chunk of bytes out of `buffer`, updating its internal position.
 *
 * @param buffer Stream to read from. The structure retains ownership of its data pointer.
 * @param out Destination buffer that receives the bytes.
 * @param length Requested byte count. The function may return fewer bytes on EOF.
 * @return `true` when at least one byte was copied, otherwise `false`.
 */
bool sample_buffer_read(SampleBuffer *buffer, void *out, size_t length);

/**
 * Converts a result code to a human-readable string.
 *
 * @param result Result value previously returned from a sample API call.
 * @returns A static string describing `result`.
 */
const char *sample_result_string(sample_result_t result);

#ifdef __cplusplus
} /* extern "C" */
#endif
