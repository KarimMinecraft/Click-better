// Click Better — mod Geode untuk Geometry Dash
// Memainkan suara click saat tombol ditekan dan suara release saat dilepas.
// Mendukung clickpack bergaya ZCB3 (folder clicks / releases / softclicks / ...).

#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <random>
#include <string>
#include <unordered_map>
#include <vector>

using namespace geode::prelude;
namespace fs = std::filesystem;

namespace {

    // Folder yang dikenali di dalam sebuah pack.
    const std::vector<std::string> kFolders = {
        "clicks", "releases",
        "softclicks", "softreleases",
        "hardclicks", "hardreleases",
        "microclicks", "microreleases",
    };

    bool isAudioFile(fs::path const& p) {
        auto ext = p.extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return ext == ".wav" || ext == ".mp3" || ext == ".ogg" || ext == ".flac";
    }

    std::vector<std::string> listAudio(fs::path const& dir) {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::is_directory(dir, ec)) return out;
        for (auto const& entry : fs::directory_iterator(dir, ec)) {
            if (entry.is_regular_file(ec) && isAudioFile(entry.path())) {
                out.push_back(geode::utils::string::pathToString(entry.path()));
            }
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    struct ClickPack {
        std::unordered_map<std::string, std::vector<std::string>> sounds;
        std::unordered_map<std::string, int> lastIndex;

        bool has(std::string const& key) const {
            auto it = sounds.find(key);
            return it != sounds.end() && !it->second.empty();
        }

        bool empty() const {
            for (auto const& [k, v] : sounds) {
                if (!v.empty()) return false;
            }
            return true;
        }
    };

    ClickPack g_pack;
    std::mt19937 g_rng{std::random_device{}()};

    // Dipakai untuk menentukan kategori click (soft / normal).
    double g_lastClickTime = -1000.0;
    std::string g_lastCategory = "";  // "", "soft", "hard", "micro"

    double nowSeconds() {
        using namespace std::chrono;
        return duration<double>(steady_clock::now().time_since_epoch()).count();
    }

    fs::path packsDir() {
        return Mod::get()->getConfigDir() / "packs";
    }

    void loadPack(std::string const& name) {
        g_pack = ClickPack{};

        auto root = packsDir() / name;
        std::error_code ec;
        if (!fs::is_directory(root, ec)) {
            log::warn("Pack '{}' tidak ditemukan di {}", name, geode::utils::string::pathToString(root));
            return;
        }

        for (auto const& folder : kFolders) {
            g_pack.sounds[folder] = listAudio(root / folder);
        }

        // Cadangan: kalau pack tidak punya folder, pakai file langsung di root.
        // File yang namanya mengandung "release" dianggap release, sisanya click.
        if (!g_pack.has("clicks") && !g_pack.has("releases")) {
            for (auto const& file : listAudio(root)) {
                auto lower = fs::path(file).filename().string();
                std::transform(lower.begin(), lower.end(), lower.begin(), [](unsigned char c) {
                    return static_cast<char>(std::tolower(c));
                });
                if (lower.find("release") != std::string::npos) {
                    g_pack.sounds["releases"].push_back(file);
                } else {
                    g_pack.sounds["clicks"].push_back(file);
                }
            }
        }

        log::info(
            "Pack '{}' dimuat: {} click, {} release, {} softclick, {} softrelease",
            name,
            g_pack.sounds["clicks"].size(),
            g_pack.sounds["releases"].size(),
            g_pack.sounds["softclicks"].size(),
            g_pack.sounds["softreleases"].size()
        );
    }

    // Pilih file acak dari folder, hindari file yang sama dua kali berturut-turut.
    std::string pickSound(std::string const& key) {
        auto it = g_pack.sounds.find(key);
        if (it == g_pack.sounds.end() || it->second.empty()) return "";

        auto const& list = it->second;
        if (list.size() == 1) return list[0];

        std::uniform_int_distribution<int> dist(0, static_cast<int>(list.size()) - 1);
        int idx = dist(g_rng);
        int& last = g_pack.lastIndex[key];
        if (idx == last) idx = (idx + 1) % static_cast<int>(list.size());
        last = idx;
        return list[idx];
    }

    void playSound(std::string const& path, float volume) {
        if (path.empty()) return;

        float variation = static_cast<float>(Mod::get()->getSettingValue<double>("pitch-variation"));
        float speed = 1.f;
        if (variation > 0.f) {
            std::uniform_real_distribution<float> dist(-variation, variation);
            speed += dist(g_rng);
        }

        FMODAudioEngine::sharedEngine()->playEffect(path, speed, 0.f, volume);
    }

    // Fallback kategori: kalau folder soft tidak ada, pakai folder normal.
    std::string resolveKey(std::string const& category, bool release) {
        std::string base = release ? "releases" : "clicks";
        if (category.empty()) return base;
        std::string key = category + base;  // contoh: "softclicks"
        return g_pack.has(key) ? key : base;
    }

}

$on_mod(Loaded) {
    std::error_code ec;
    fs::create_directories(packsDir(), ec);

    loadPack(Mod::get()->getSettingValue<std::string>("pack"));

    listenForSettingChanges<std::string>("pack", [](std::string value) {
        loadPack(value);
    });
}

class $modify(ClickBetterLayer, GJBaseGameLayer) {
    void handleButton(bool down, int button, bool isPlayer1) {
        GJBaseGameLayer::handleButton(down, button, isPlayer1);

        auto mod = Mod::get();
        if (!mod->getSettingValue<bool>("enabled")) return;
        if (!isPlayer1 && !mod->getSettingValue<bool>("player2")) return;
        if (g_pack.empty()) return;

        float clickVolume = static_cast<float>(mod->getSettingValue<double>("click-volume"));
        float releaseVolume = static_cast<float>(mod->getSettingValue<double>("release-volume"));

        if (down) {
            double now = nowSeconds();
            double gap = now - g_lastClickTime;
            g_lastClickTime = now;

            double softGap = mod->getSettingValue<double>("soft-gap");
            g_lastCategory = (gap < softGap) ? "soft" : "";

            playSound(pickSound(resolveKey(g_lastCategory, false)), clickVolume);
        } else {
            if (!mod->getSettingValue<bool>("play-release")) return;
            // Release memakai kategori yang sama dengan click sebelumnya.
            playSound(pickSound(resolveKey(g_lastCategory, true)), releaseVolume);
        }
    }
};
