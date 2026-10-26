// Convert PNG/JPEG images to a multi-resolution Windows ICO file.
// Linux/Termux use libpng and libjpeg; Windows uses the built-in WIC decoder.
// Build on Linux/Termux: c++ -std=c++17 -O2 png2icon.cpp -lpng -ljpeg -o png2icon
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <setjmp.h>
#include <stdexcept>
#include <string>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <wincodec.h>
#else
#include <jpeglib.h>
#include <png.h>
#endif

struct Image {
    unsigned width = 0;
    unsigned height = 0;
    std::vector<std::uint8_t> rgba;
};

#ifndef _WIN32
static Image read_png(const std::string& path) {
    png_image png{};
    png.version = PNG_IMAGE_VERSION;
    if (!png_image_begin_read_from_file(&png, path.c_str()))
        throw std::runtime_error(png.message[0] ? png.message : "could not read PNG");

    png.format = PNG_FORMAT_RGBA;
    Image image{png.width, png.height, {}};
    image.rgba.resize(PNG_IMAGE_SIZE(png));
    if (!png_image_finish_read(&png, nullptr, image.rgba.data(), 0, nullptr)) {
        std::string message = png.message[0] ? png.message : "could not decode PNG";
        png_image_free(&png);
        throw std::runtime_error(message);
    }
    png_image_free(&png);
    return image;
}

struct JpegError {
    jpeg_error_mgr base;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto* error = reinterpret_cast<JpegError*>(cinfo->err);
    (*cinfo->err->format_message)(cinfo, error->message);
    longjmp(error->jump, 1);
}

static Image read_jpeg(const std::string& path) {
    FILE* file = std::fopen(path.c_str(), "rb");
    if (!file) throw std::runtime_error("could not open input file");

    jpeg_decompress_struct decoder{};
    JpegError error{};
    decoder.err = jpeg_std_error(&error.base);
    error.base.error_exit = jpeg_error_exit;
    if (setjmp(error.jump)) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        throw std::runtime_error(error.message[0] ? error.message : "could not decode JPEG");
    }

    jpeg_create_decompress(&decoder);
    jpeg_stdio_src(&decoder, file);
    jpeg_read_header(&decoder, TRUE);
    decoder.out_color_space = JCS_RGB;
    jpeg_start_decompress(&decoder);

    Image image;
    image.width = decoder.output_width;
    image.height = decoder.output_height;
    if (!image.width || !image.height || decoder.output_components != 3) {
        jpeg_destroy_decompress(&decoder);
        std::fclose(file);
        throw std::runtime_error("unsupported JPEG pixel format");
    }
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    std::vector<JSAMPLE> row(static_cast<std::size_t>(image.width) * 3);
    while (decoder.output_scanline < decoder.output_height) {
        JSAMPROW row_ptr = row.data();
        const unsigned y = decoder.output_scanline;
        jpeg_read_scanlines(&decoder, &row_ptr, 1);
        for (unsigned x = 0; x < image.width; ++x) {
            const std::size_t dst = (static_cast<std::size_t>(y) * image.width + x) * 4;
            image.rgba[dst] = row[x * 3];
            image.rgba[dst + 1] = row[x * 3 + 1];
            image.rgba[dst + 2] = row[x * 3 + 2];
            image.rgba[dst + 3] = 255;
        }
    }
    jpeg_finish_decompress(&decoder);
    jpeg_destroy_decompress(&decoder);
    std::fclose(file);
    return image;
}

#else

template <typename T>
struct ComReleaser {
    void operator()(T* object) const { if (object) object->Release(); }
};

class ComApartment {
public:
    ComApartment() {
        const HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        if (FAILED(result) && result != RPC_E_CHANGED_MODE)
            throw std::runtime_error("could not initialize Windows COM");
        initialized_ = SUCCEEDED(result);
    }
    ~ComApartment() { if (initialized_) CoUninitialize(); }
private:
    bool initialized_ = false;
};

static std::wstring to_wide(const std::string& text) {
    const int count = MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, nullptr, 0);
    if (!count) throw std::runtime_error("invalid input filename");
    std::wstring result(static_cast<std::size_t>(count), L'\0');
    MultiByteToWideChar(CP_ACP, 0, text.c_str(), -1, result.data(), count);
    result.resize(static_cast<std::size_t>(count - 1));
    return result;
}

static Image read_wic_image(const std::string& path) {
    ComApartment apartment;
    IWICImagingFactory* raw_factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&raw_factory));
    if (FAILED(hr)) throw std::runtime_error("could not create Windows Imaging Component factory");
    std::unique_ptr<IWICImagingFactory, ComReleaser<IWICImagingFactory>> factory(raw_factory);

    IWICBitmapDecoder* raw_decoder = nullptr;
    const std::wstring filename = to_wide(path);
    hr = factory->CreateDecoderFromFilename(filename.c_str(), nullptr, GENERIC_READ,
                                            WICDecodeMetadataCacheOnLoad, &raw_decoder);
    if (FAILED(hr)) throw std::runtime_error("could not decode image (PNG/JPEG expected)");
    std::unique_ptr<IWICBitmapDecoder, ComReleaser<IWICBitmapDecoder>> decoder(raw_decoder);

    IWICBitmapFrameDecode* raw_frame = nullptr;
    hr = decoder->GetFrame(0, &raw_frame);
    if (FAILED(hr)) throw std::runtime_error("could not read image frame");
    std::unique_ptr<IWICBitmapFrameDecode, ComReleaser<IWICBitmapFrameDecode>> frame(raw_frame);

    IWICFormatConverter* raw_converter = nullptr;
    hr = factory->CreateFormatConverter(&raw_converter);
    if (FAILED(hr)) throw std::runtime_error("could not create image format converter");
    std::unique_ptr<IWICFormatConverter, ComReleaser<IWICFormatConverter>> converter(raw_converter);
    hr = converter->Initialize(frame.get(), GUID_WICPixelFormat32bppRGBA,
                               WICBitmapDitherTypeNone, nullptr, 0.0,
                               WICBitmapPaletteTypeCustom);
    if (FAILED(hr)) throw std::runtime_error("could not convert image to RGBA");

    Image image;
    hr = converter->GetSize(&image.width, &image.height);
    if (FAILED(hr) || !image.width || !image.height)
        throw std::runtime_error("invalid image dimensions");
    image.rgba.resize(static_cast<std::size_t>(image.width) * image.height * 4);
    hr = converter->CopyPixels(nullptr, image.width * 4,
                               static_cast<UINT>(image.rgba.size()), image.rgba.data());
    if (FAILED(hr)) throw std::runtime_error("could not read image pixels");
    return image;
}
#endif

static Image read_image(const std::string& path) {
    const auto dot = path.find_last_of('.');
    std::string ext = dot == std::string::npos ? "" : path.substr(dot);
    std::transform(ext.begin(), ext.end(), ext.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (ext != ".png" && ext != ".jpg" && ext != ".jpeg")
        throw std::runtime_error("input must have a .png, .jpg, or .jpeg extension");
#ifdef _WIN32
    return read_wic_image(path);
#else
    if (ext == ".png") return read_png(path);
    return read_jpeg(path);
#endif
}

static std::vector<std::uint8_t> resize_rgba(const Image& src, unsigned size) {
    std::vector<std::uint8_t> out(static_cast<std::size_t>(size) * size * 4);
    for (unsigned y = 0; y < size; ++y) {
        const double fy = std::clamp((y + 0.5) * src.height / size - 0.5,
                                     0.0, static_cast<double>(src.height - 1));
        const unsigned y0 = static_cast<unsigned>(std::floor(fy));
        const unsigned y1 = std::min(src.height - 1, y0 + 1);
        const double wy = fy - y0;
        for (unsigned x = 0; x < size; ++x) {
            const double fx = std::clamp((x + 0.5) * src.width / size - 0.5,
                                         0.0, static_cast<double>(src.width - 1));
            const unsigned x0 = static_cast<unsigned>(std::floor(fx));
            const unsigned x1 = std::min(src.width - 1, x0 + 1);
            const double wx = fx - x0;
            const unsigned xs[4] = {x0, x1, x0, x1};
            const unsigned ys[4] = {y0, y0, y1, y1};
            const double weights[4] = {(1-wx)*(1-wy), wx*(1-wy), (1-wx)*wy, wx*wy};
            double alpha = 0, premul[3] = {0, 0, 0};
            for (int k = 0; k < 4; ++k) {
                const std::size_t i = (static_cast<std::size_t>(ys[k]) * src.width + xs[k]) * 4;
                const double a = src.rgba[i + 3] / 255.0;
                alpha += src.rgba[i + 3] * weights[k];
                for (int c = 0; c < 3; ++c) premul[c] += src.rgba[i + c] * a * weights[k];
            }
            const std::size_t o = (static_cast<std::size_t>(y) * size + x) * 4;
            if (alpha > 0)
                for (int c = 0; c < 3; ++c)
                    out[o + c] = static_cast<std::uint8_t>(std::clamp(std::lround(premul[c] * 255 / alpha), 0L, 255L));
            out[o + 3] = static_cast<std::uint8_t>(std::clamp(std::lround(alpha), 0L, 255L));
        }
    }
    return out;
}

static void put16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(v & 0xff); out.push_back((v >> 8) & 0xff);
}
static void put32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    put16(out, v & 0xffff); put16(out, v >> 16);
}

static std::vector<std::uint8_t> dib_image(const std::vector<std::uint8_t>& rgba, unsigned size) {
    const std::size_t xor_size = static_cast<std::size_t>(size) * size * 4;
    const std::size_t mask_stride = ((size + 31) / 32) * 4;
    const std::size_t mask_size = mask_stride * size;
    std::vector<std::uint8_t> out;
    out.reserve(40 + xor_size + mask_size);
    put32(out, 40); put32(out, size); put32(out, size * 2);
    put16(out, 1); put16(out, 32); put32(out, 0);
    put32(out, static_cast<std::uint32_t>(xor_size + mask_size));
    put32(out, 0); put32(out, 0); put32(out, 0); put32(out, 0);

    for (int y = static_cast<int>(size) - 1; y >= 0; --y) {
        for (unsigned x = 0; x < size; ++x) {
            const std::size_t i = (static_cast<std::size_t>(y) * size + x) * 4;
            out.push_back(rgba[i + 2]); out.push_back(rgba[i + 1]);
            out.push_back(rgba[i]); out.push_back(rgba[i + 3]);
        }
    }
    const std::size_t mask_begin = out.size();
    out.resize(mask_begin + mask_size, 0);
    for (unsigned row = 0; row < size; ++row) {
        const unsigned y = size - 1 - row;
        for (unsigned x = 0; x < size; ++x) {
            if (rgba[(static_cast<std::size_t>(y) * size + x) * 4 + 3] < 128)
                out[mask_begin + static_cast<std::size_t>(row) * mask_stride + x / 8] |= 0x80 >> (x % 8);
        }
    }
    return out;
}

static void write_ico(const std::string& path, const Image& source) {
    const unsigned sizes[] = {256, 128, 64, 48, 32, 16};
    std::vector<std::vector<std::uint8_t>> images;
    for (unsigned size : sizes) images.push_back(dib_image(resize_rgba(source, size), size));

    std::vector<std::uint8_t> ico;
    put16(ico, 0); put16(ico, 1); put16(ico, 6);
    std::uint32_t offset = 6 + 16 * 6;
    for (std::size_t i = 0; i < 6; ++i) {
        const unsigned size = sizes[i];
        ico.push_back(size == 256 ? 0 : static_cast<std::uint8_t>(size));
        ico.push_back(size == 256 ? 0 : static_cast<std::uint8_t>(size));
        ico.push_back(0); ico.push_back(0);
        put16(ico, 1); put16(ico, 32);
        put32(ico, static_cast<std::uint32_t>(images[i].size())); put32(ico, offset);
        offset += static_cast<std::uint32_t>(images[i].size());
    }
    for (const auto& image : images) ico.insert(ico.end(), image.begin(), image.end());
    std::ofstream file(path, std::ios::binary);
    if (!file || !file.write(reinterpret_cast<const char*>(ico.data()), static_cast<std::streamsize>(ico.size())))
        throw std::runtime_error("could not write output file");
}

int main(int argc, char** argv) {
    if (argc < 2 || argc > 3) {
        std::cerr << "Usage: " << argv[0] << " input.(png|jpg|jpeg) [output.ico]\n";
        return 2;
    }
    const std::string input = argv[1];
    std::string output;
    if (argc == 3) output = argv[2];
    else {
        output = input;
        const auto dot = output.find_last_of('.');
        const auto slash = output.find_last_of("/\\");
        if (dot != std::string::npos && (slash == std::string::npos || dot > slash)) output.resize(dot);
        output += ".ico";
    }
    try {
        const Image image = read_image(input);
        if (!image.width || !image.height) throw std::runtime_error("image is empty");
        write_ico(output, image);
        std::cout << "Created " << output << " (6 sizes)\n";
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "Could not create " << output << ": " << e.what() << '\n';
        return 1;
    }
}
