#include "esp32_camera.h"

#include <esp_heap_caps.h>
#include <esp_log.h>
#include <freertos/task.h>

#include <array>
#include <cstring>
#include <utility>

#include "board.h"
#include "esp_timer.h"
#include "jpg/image_to_jpeg.h"
#include "system_info.h"

#define TAG "Esp32Camera"

std::expected<std::string, std::string> Esp32Camera::Explain(const std::string& question) {
    OwnedJpeg owned_snapshot;
    {
        std::lock_guard<std::mutex> lock(mcp_snapshot_mutex_);
        owned_snapshot = std::move(mcp_snapshot_);
    }
    if (explain_url_.empty()) {
        return std::unexpected("Image explain URL or token is not set");
    }
    if (!owned_snapshot && current_fb_ == nullptr) {
        return std::unexpected("No camera frame captured");
    }

    // DVP sensors on this board produce JPEG directly. Send that frame with a
    // fixed Content-Length instead of chunked transfer encoding: some image
    // explain endpoints accept the TCP connection but never consume a chunked
    // multipart request, leaving the MCP call pending indefinitely.
    if (owned_snapshot || current_fb_->format == PIXFORMAT_JPEG) {
        const uint8_t* jpeg_data = owned_snapshot ? owned_snapshot.data : current_fb_->buf;
        const size_t jpeg_length = owned_snapshot ? owned_snapshot.length : current_fb_->len;
        const int jpeg_width = owned_snapshot ? owned_snapshot.width : current_fb_->width;
        const int jpeg_height = owned_snapshot ? owned_snapshot.height : current_fb_->height;
        auto network = Board::GetInstance().GetNetwork();
        auto http = network->CreateHttp(3);
        http->SetTimeout(20000);

        auto close_http = [&]() {
            http->Close();
        };

        const std::string boundary = "----ESP32_CAMERA_BOUNDARY";
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";

        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header +=
            "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";

        const std::string multipart_footer = "\r\n--" + boundary + "--\r\n";
        const size_t content_length = question_field.size() + file_header.size() +
                                      jpeg_length + multipart_footer.size();

        http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
        http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
        if (!explain_token_.empty()) {
            http->SetHeader("Authorization", "Bearer " + explain_token_);
        }
        http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
        http->SetHeader("Content-Length", std::to_string(content_length));

        // An engaged but empty content value makes HttpClient use raw writes
        // after Open(), while the explicit Content-Length describes the body.
        http->SetContent(std::string{});
        ESP_LOGI(TAG, "JPEG upload begin: image=%dx%d %zu bytes, body=%zu bytes", jpeg_width,
                 jpeg_height, jpeg_length,
                 content_length);
        auto opened = http->Open("POST", explain_url_);
        if (!opened) {
            const std::string error = opened.error().ToString();
            ESP_LOGE(TAG, "Failed to connect to explain URL: %s", error.c_str());
            return std::unexpected("Failed to connect to explain URL: " + error);
        }
        std::string upload_error;
        auto write_part = [&http, &upload_error](const char* name, const char* data,
                                                 size_t length) {
            auto write_result = http->Write(data, length);
            if (!write_result) {
                upload_error = "JPEG upload " + std::string(name) + " failed: " +
                               write_result.error().ToString();
                ESP_LOGE(TAG, "%s", upload_error.c_str());
                return false;
            }

            const int written = *write_result;
            if (written != static_cast<int>(length)) {
                upload_error = "JPEG upload " + std::string(name) + " was partial";
                ESP_LOGE(TAG, "JPEG upload %s failed: wrote %d/%zu bytes", name, written,
                         length);
                return false;
            }
            ESP_LOGD(TAG, "JPEG upload %s: %zu bytes", name, length);
            return true;
        };

        const bool uploaded =
            write_part("question", question_field.data(), question_field.size()) &&
            write_part("header", file_header.data(), file_header.size()) &&
            write_part("image", reinterpret_cast<const char*>(jpeg_data), jpeg_length) &&
            write_part("footer", multipart_footer.data(), multipart_footer.size());
        if (!uploaded) {
            close_http();
            if (upload_error.empty()) {
                upload_error = "Failed to upload photo";
            }
            return std::unexpected(std::move(upload_error));
        }
        ESP_LOGI(TAG, "JPEG upload complete; waiting for response");
        auto status_code = http->GetStatusCode();
        if (!status_code) {
            ESP_LOGE(TAG, "Failed to read HTTP status: %s",
                     status_code.error().ToString().c_str());
            const std::string error = status_code.error().ToString();
            close_http();
            return std::unexpected("Failed to read image explain HTTP status: " + error);
        }
        if (*status_code != 200) {
            ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
            close_http();
            return std::unexpected("Image explain returned HTTP " +
                                   std::to_string(*status_code));
        }

        constexpr size_t kMaxResponseBytes = 64 * 1024;
        std::string result;
        const size_t declared_length = http->GetBodyLength();
        if (declared_length > kMaxResponseBytes) {
            close_http();
            return std::unexpected("Image explain response is too large");
        }
        if (declared_length > 0) {
            result.reserve(declared_length);
        }

        std::array<char, 1024> response_buffer;
        while (true) {
            auto bytes_read = http->Read(response_buffer.data(), response_buffer.size());
            if (!bytes_read) {
                const std::string error = bytes_read.error().ToString();
                close_http();
                return std::unexpected("Failed to read image explain response: " + error);
            }
            if (*bytes_read == 0) {
                break;
            }
            if (result.size() + static_cast<size_t>(*bytes_read) > kMaxResponseBytes) {
                close_http();
                return std::unexpected("Image explain response is too large");
            }
            result.append(response_buffer.data(), static_cast<size_t>(*bytes_read));
        }
        close_http();
        if (result.empty()) {
            ESP_LOGE(TAG, "Image explain returned an empty response");
            return std::unexpected("Image explain returned an empty response");
        }
        ESP_LOGI(TAG, "Explain image size=%dx%d %zu bytes, question=%s\n%s", jpeg_width,
                 jpeg_height, jpeg_length, question.c_str(), result.c_str());
        return result;
    }

    // Create local JPEG queue
    QueueHandle_t jpeg_queue = xQueueCreate(40, sizeof(JpegChunk));
    if (jpeg_queue == nullptr) {
        ESP_LOGE(TAG, "Failed to create JPEG queue");
        return std::unexpected("Failed to create JPEG queue");
    }

    // Start encoding thread
    encoder_thread_ = std::thread([this, jpeg_queue]() {
        int64_t start_time = esp_timer_get_time();
        uint16_t w = current_fb_->width;
        uint16_t h = current_fb_->height;
        v4l2_pix_fmt_t enc_fmt;
        switch (current_fb_->format) {
            case PIXFORMAT_RGB565:
                enc_fmt = V4L2_PIX_FMT_RGB565;
                break;
            case PIXFORMAT_YUV422:
                enc_fmt = V4L2_PIX_FMT_YUYV;  // YUV422 is actually YUYV format
                break;
            case PIXFORMAT_YUV420:
                enc_fmt = V4L2_PIX_FMT_YUV420;
                break;
            case PIXFORMAT_GRAYSCALE:
                enc_fmt = V4L2_PIX_FMT_GREY;
                break;
            case PIXFORMAT_JPEG:
                enc_fmt = V4L2_PIX_FMT_JPEG;
                break;
            case PIXFORMAT_RGB888:
                enc_fmt = V4L2_PIX_FMT_RGB24;
                break;
            default:
                ESP_LOGE(TAG, "Unsupported pixel format: %d", current_fb_->format);
                return;
        }

        // Use encode buffer for RGB565, otherwise use original frame buffer
        uint8_t* jpeg_src_buf = current_fb_->buf;
        size_t jpeg_src_len = current_fb_->len;
        if (current_fb_->format == PIXFORMAT_RGB565 && encode_buf_ != nullptr) {
            jpeg_src_buf = encode_buf_;
            jpeg_src_len = encode_buf_size_;
        }

        bool ok = image_to_jpeg_cb(
            jpeg_src_buf, jpeg_src_len, w, h, enc_fmt, 80,
            [](void* arg, size_t index, const void* data, size_t len) -> size_t {
                auto jpeg_queue = static_cast<QueueHandle_t>(arg);
                JpegChunk chunk = {.data = nullptr, .len = len};
                if (index == 0 && data != nullptr && len > 0) {
                    chunk.data = (uint8_t*)heap_caps_aligned_alloc(
                        16, len, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
                    if (chunk.data == nullptr) {
                        ESP_LOGE(TAG, "Failed to allocate %zu bytes for JPEG chunk", len);
                        chunk.len = 0;
                    } else {
                        memcpy(chunk.data, data, len);
                    }
                } else {
                    chunk.len = 0;  // Sentinel or error
                }
                xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
                return len;
            },
            jpeg_queue);

        if (!ok) {
            JpegChunk chunk = {.data = nullptr, .len = 0};
            xQueueSend(jpeg_queue, &chunk, portMAX_DELAY);
        }
        int64_t end_time = esp_timer_get_time();
        ESP_LOGI(TAG, "JPEG encoding time: %ld ms", int((end_time - start_time) / 1000));
    });

    auto network = Board::GetInstance().GetNetwork();
    auto http = network->CreateHttp(3);
    http->SetTimeout(20000);
    std::string boundary = "----ESP32_CAMERA_BOUNDARY";

    http->SetHeader("Device-Id", SystemInfo::GetMacAddress().c_str());
    http->SetHeader("Client-Id", Board::GetInstance().GetUuid().c_str());
    if (!explain_token_.empty()) {
        http->SetHeader("Authorization", "Bearer " + explain_token_);
    }
    http->SetHeader("Content-Type", "multipart/form-data; boundary=" + boundary);
    http->SetHeader("Transfer-Encoding", "chunked");
    if (auto opened = http->Open("POST", explain_url_); !opened) {
        ESP_LOGE(TAG, "Failed to connect to explain URL: %s", opened.error().ToString().c_str());
        encoder_thread_.join();
        JpegChunk chunk;
        while (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) == pdPASS) {
            if (chunk.data != nullptr) {
                heap_caps_free(chunk.data);
            } else {
                break;
            }
        }
        vQueueDelete(jpeg_queue);
        return std::unexpected("Failed to connect to explain URL");
    }

    {
        std::string question_field;
        question_field += "--" + boundary + "\r\n";
        question_field += "Content-Disposition: form-data; name=\"question\"\r\n";
        question_field += "\r\n";
        question_field += question + "\r\n";
        http->Write(question_field.c_str(), question_field.size());
    }
    {
        std::string file_header;
        file_header += "--" + boundary + "\r\n";
        file_header += "Content-Disposition: form-data; name=\"file\"; filename=\"camera.jpg\"\r\n";
        file_header += "Content-Type: image/jpeg\r\n";
        file_header += "\r\n";
        http->Write(file_header.c_str(), file_header.size());
    }

    size_t total_sent = 0;
    bool saw_terminator = false;
    while (true) {
        JpegChunk chunk;
        if (xQueueReceive(jpeg_queue, &chunk, portMAX_DELAY) != pdPASS) {
            ESP_LOGE(TAG, "Failed to receive JPEG chunk");
            break;
        }
        if (chunk.data == nullptr) {
            saw_terminator = true;
            break;
        }
        http->Write((const char*)chunk.data, chunk.len);
        total_sent += chunk.len;
        heap_caps_free(chunk.data);
    }
    encoder_thread_.join();
    vQueueDelete(jpeg_queue);

    if (!saw_terminator || total_sent == 0) {
        ESP_LOGE(TAG, "JPEG encoder failed or produced empty output");
        return std::unexpected("Failed to encode image to JPEG");
    }

    {
        std::string multipart_footer;
        multipart_footer += "\r\n--" + boundary + "--\r\n";
        http->Write(multipart_footer.c_str(), multipart_footer.size());
    }
    http->Write("", 0);

    auto status_code = http->GetStatusCode();
    if (!status_code) {
        ESP_LOGE(TAG, "Failed to read HTTP status: %s", status_code.error().ToString().c_str());
        http->Close();
        return std::unexpected("Failed to upload photo");
    }
    if (*status_code != 200) {
        ESP_LOGE(TAG, "Failed to upload photo, status code: %d", *status_code);
        http->Close();
        return std::unexpected("Failed to upload photo");
    }

    std::string result = http->ReadAll();
    http->Close();

    size_t remain_stack_size = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGI(TAG,
             "Explain image size=%dx%d, compressed size=%d, remain stack size=%d, question=%s\n%s",
             current_fb_->width, current_fb_->height, (int)total_sent, (int)remain_stack_size,
             question.c_str(), result.c_str());
    return result;
}
