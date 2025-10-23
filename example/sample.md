# API Documentation

## Macros

- [`__GCC_HAVE_DWARF2_CFI_ASM`](#macro-__gcc_have_dwarf2_cfi_asm)
- [`SAMPLE_VERSION_MAJOR`](#macro-sample_version_major)
- [`SAMPLE_VERSION_MINOR`](#macro-sample_version_minor)
- [`SAMPLE_ARRAY_LENGTH`](#macro-sample_array_length)
- [`SAMPLE_RETRY`](#macro-sample_retry)

## Types

- [`sample_result`](#type-sample_result)
- [`sample_result_t`](#type-typedef-sample_result_t)
- [`sample_mode`](#type-sample_mode)
- [`sample_mode_t`](#type-typedef-sample_mode_t)
- [`SampleBuffer`](#type-samplebuffer)
- [`SampleConfig`](#type-sampleconfig)
- [`sample_logger_fn`](#type-typedef-sample_logger_fn)

## Functions

- [`sample_init_config`](#function-sample_init_config)
- [`sample_set_logger`](#function-sample_set_logger)
- [`sample_run`](#function-sample_run)
- [`sample_buffer_read`](#function-sample_buffer_read)
- [`sample_result_string`](#function-sample_result_string)

## File: example/sample.h

@file sample.h
@brief Example declarations used to showcase the documentation generator.

This header defines a handful of macros, typedefs, and functions that exercise
the generator's ability to understand docstrings, parameter lists, and code
blocks. Nothing here is production ready — it simply demonstrates formatting
and annotation styles the tool can consume.

<a id="macro-__gcc_have_dwarf2_cfi_asm"></a>
### Macro: `__GCC_HAVE_DWARF2_CFI_ASM`

```c
#define __GCC_HAVE_DWARF2_CFI_ASM __GCC_HAVE_DWARF2_CFI_ASM 1
```

---

<a id="macro-sample_version_major"></a>
### Macro: `SAMPLE_VERSION_MAJOR`

Major version of the sample API.

```c
#define SAMPLE_VERSION_MAJOR SAMPLE_VERSION_MAJOR 1
```


*Defined at*: `example/sample.h:24`

---

<a id="macro-sample_version_minor"></a>
### Macro: `SAMPLE_VERSION_MINOR`

Minor version of the sample API.

```c
#define SAMPLE_VERSION_MINOR SAMPLE_VERSION_MINOR 0
```


*Defined at*: `example/sample.h:29`

---

<a id="macro-sample_array_length"></a>
### Macro: `SAMPLE_ARRAY_LENGTH`

Expands to the total number of elements in a static array.

#### Parameters

**arr** — Static array whose element count should be computed.

```c
#define SAMPLE_ARRAY_LENGTH SAMPLE_ARRAY_LENGTH ( arr) ( sizeof ( arr) / sizeof ( ( arr) [ 0]))
```


*Defined at*: `example/sample.h:36`

---

<a id="macro-sample_retry"></a>
### Macro: `SAMPLE_RETRY`

Wraps a statement with a retry loop that exits after `max_attempts`.

#### Parameters

**expr** — Statement that returns `true` on success.

**max_attempts** — Maximum number of iterations before giving up.
  
  Example usage:
  
  ```c
  SAMPLE_RETRY(sample_connect(), 3) {
      sample_sleep(10);
  }
  ```

```c
#define SAMPLE_RETRY SAMPLE_RETRY ( expr, max_attempts) for ( unsigned _sample_try = 0; _sample_try < ( unsigned) ( max_attempts); ++ _sample_try) if ( expr) break; else
```


*Defined at*: `example/sample.h:51`

---

<a id="type-sample_result"></a>
### : `sample_result`

Enumerates the possible results returned by sample operations.

- `SAMPLE_OK = 0`
- `SAMPLE_ERR_INVALID_ARGUMENT = 1`
- `SAMPLE_ERR_IO = 2`
- `SAMPLE_ERR_TIMEOUT = 3`


*Defined at*: `example/sample.h:60`

---

<a id="type-typedef-sample_result_t"></a>
### Typedef: `sample_result_t`

Enumerates the possible results returned by sample operations.

```c
typedef enum sample_result sample_result_t;
```


*Defined at*: `example/sample.h:65`

---

<a id="type-sample_mode"></a>
### : `sample_mode`

Describes how the processor should interpret incoming data.

- `SAMPLE_MODE_BINARY = 0`
- `SAMPLE_MODE_TEXT = 1`
- `SAMPLE_MODE_COMPRESSED = 2`


*Defined at*: `example/sample.h:70`

---

<a id="type-typedef-sample_mode_t"></a>
### Typedef: `sample_mode_t`

Describes how the processor should interpret incoming data.

```c
typedef enum sample_mode sample_mode_t;
```


*Defined at*: `example/sample.h:74`

---

<a id="type-samplebuffer"></a>
### : `SampleBuffer`

Opaque handle that callers use to stream bytes into the processing pipeline.

- `const int * data;`
- `int length;`
- `int position;`


*Defined at*: `example/sample.h:79`

---

<a id="type-sampleconfig"></a>
### : `SampleConfig`

Configuration structure passed to `sample_run`.

#### Note

Call `sample_init_config` to populate sensible defaults before making
manual adjustments.

- `sample_mode_t mode;`
- `int enable_logging;`
- `int max_batch_size;`
- `const char * log_path;`


*Defined at*: `example/sample.h:91`

---

<a id="type-typedef-sample_logger_fn"></a>
### Typedef: `sample_logger_fn`

Function pointer invoked whenever the processor emits a log message.

#### Parameters

**level** — Severity string such as `"info"` or `"error"`.

**message** — Null-terminated log payload.

**user_data** — Custom pointer specified during `sample_set_logger`.

```c
typedef void (*)(const char *, const char *, void *) sample_logger_fn;
```


*Defined at*: `example/sample.h:105`

---

<a id="function-sample_init_config"></a>
### Function: `sample_init_config`

Initializes `config` with safe defaults.

#### Parameters

**config** — Structure to initialize. Must not be `NULL`.

```c
void sample_init_config(SampleConfig *config);
```


*Defined at*: `example/sample.h:112`

---

<a id="function-sample_set_logger"></a>
### Function: `sample_set_logger`

Registers a callback to receive log messages from the sample processor.

#### Parameters

**logger** — Function invoked for each emitted log line. Pass `NULL` to disable logging.

**user_data** — Pointer forwarded to every invocation of `logger`.

```c
void sample_set_logger(sample_logger_fn logger, void *user_data);
```


*Defined at*: `example/sample.h:120`

---

<a id="function-sample_run"></a>
### Function: `sample_run`

Processes a block of data using the provided configuration.

#### Parameters

**config** — Active configuration generated via `sample_init_config`.

**input** — Pointer to the bytes to process. The buffer is not owned.

**length** — Number of bytes available at `input`.



#### Returns

A `sample_result_t` describing success or failure.

#### Note

The function returns immediately when `length` is zero.

#### Warning

Passing a `NULL` pointer while `length` is non-zero triggers `SAMPLE_ERR_INVALID_ARGUMENT`.

```c
sample_result_t sample_run(const SampleConfig *config, const int *input, int length);
```


*Defined at*: `example/sample.h:133`

---

<a id="function-sample_buffer_read"></a>
### Function: `sample_buffer_read`

Reads a chunk of bytes out of `buffer`, updating its internal position.

#### Parameters

**buffer** — Stream to read from. The structure retains ownership of its data pointer.

**out** — Destination buffer that receives the bytes.

**length** — Requested byte count. The function may return fewer bytes on EOF.



#### Returns

`true` when at least one byte was copied, otherwise `false`.

```c
int sample_buffer_read(SampleBuffer *buffer, void *out, int length);
```


*Defined at*: `example/sample.h:143`

---

<a id="function-sample_result_string"></a>
### Function: `sample_result_string`

Converts a result code to a human-readable string.

#### Parameters

**result** — Result value previously returned from a sample API call.



#### Returns

A static string describing `result`.

```c
const char * sample_result_string(sample_result_t result);
```


*Defined at*: `example/sample.h:151`

---

