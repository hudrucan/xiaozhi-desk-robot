#include <esp_err.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief Decodes a JPEG image from memory to raw RGB565 pixel data
 *
 * This function decodes a JPEG image using esp_new_jpeg.
 *
 * @param[in] src Pointer to the JPEG bitstream in memory
 * @param[in] src_len Length of the JPEG bitstream in bytes
 * @param[out] out Pointer to a buffer pointer that will be set to the decoded image data.
 *             This buffer is allocated internally and MUST be freed by the caller using heap_caps_free().
 * @param[out] out_len Pointer to a variable that will receive the size of the decoded image data in bytes
 * @param[out] width Pointer to a variable that will receive the image width in pixels
 * @param[out] height Pointer to a variable that will receive the image height in pixels
 * @param[out] stride Pointer to a variable that will receive the image stride in bytes
 *
 * @return ESP_OK on successful decoding
 * @return ESP_ERR_INVALID_ARG on invalid parameters
 * @return ESP_ERR_NO_MEM on memory allocation failure
 * @return ESP_FAIL on failure
 *
 * @attention Memory Management for `*out`:
 *            - The function allocates memory for the decoded image internally
 *            - On success, the caller takes ownership of this memory and SHOULD free it using heap_caps_free()
 *            - On failure, `*out` is guaranteed to be NULL and no freeing is required
 *            - Example usage:
 *              @code{.c}
 *              uint8_t *image = NULL;
 *              size_t len, width, height;
 *              if (jpeg_to_image(jpeg_data, jpeg_len, &image, &len, &width, &height)) {
 *                  // Use image data...
 *                  heap_caps_free(image);  // Critical: use heap_caps_free
 *              }
 *              @endcode
 *
 * @note The decoded image format is always RGB565 (2 bytes per pixel).
 *
 */
esp_err_t jpeg_to_image(const uint8_t* src, size_t src_len, uint8_t** out, size_t* out_len, size_t* width,
                        size_t* height, size_t* stride);

/**
 * @brief Decode JPEG to an explicitly scaled RGB565 output.
 *
 * Scale dimensions must be multiples of 8. Pass zero for both dimensions to
 * preserve the JPEG's original size.
 */
esp_err_t jpeg_to_image_scaled(const uint8_t* src, size_t src_len, uint8_t** out,
                               size_t* out_len, size_t* width, size_t* height,
                               size_t* stride, size_t scale_width, size_t scale_height);

#ifdef __cplusplus
}
#endif
