#pragma once

#include <string>

// Dark mode for the bundled Flutter web UI (Home / Device pages).
//
// The Flutter app ships with a hard-wired light theme. When a page is opened with
// dark_mode=1, a dark variant derived from the installed flutter_web is served:
//  - index.html gets a small script that switches the entry point to the dark bundle,
//  - main.<hash>.dark1.js is main.<hash>.js with every color literal remapped using
//    the same light -> dark mapping as the native GUI (StateColor.cpp, gDarkColors),
//  - assets/assets/svgs_dark1/** are the SVG icons with their colors remapped,
//  - assets/assets/dark1/** are prebuilt dark placeholder images from
//    resources/web/flutter_web_dark/, used only while the source image is unchanged
//    (see scripts/flutter_web_dark.py).
// The local HttpServer derives these files on request. The connection gateway
// (snapmaker_control) serves the pages from web/flutter_web next to its executable
// instead, so prepare_gateway_root() writes them into a copy of the gateway.
namespace Slic3r { namespace GUI { namespace FlutterWebDark {

enum class Kind { None, IndexHtml, Bundle, Svg };

struct Resolved
{
    std::string path;          // file to read from disk
    Kind        kind = Kind::None;
};

// Map a requested file path inside flutter_web to the file to read and the transform to apply.
Resolved resolve(const std::string &file_path);

// Apply the transform for `kind` to the content read from `source_path`.
// The bundle transform is cached, so repeated page loads stay cheap.
std::string apply(Kind kind, const std::string &source_path, std::string content);

// Copy the gateway executable at `cli_path` and its web/flutter_web into the data
// directory, adding the dark files next to the light ones; the page URL then picks
// the theme. Rebuilt only when the gateway or flutter_web change. Returns the path
// of the copied executable, or an empty string to keep using `cli_path`.
std::string prepare_gateway_root(const std::string &cli_path);

// Pure transforms, exposed for testing.
std::string remap_bundle(const std::string &js);
std::string remap_svg(const std::string &svg);
std::string patch_index_html(const std::string &html);

}}} // namespace Slic3r::GUI::FlutterWebDark
