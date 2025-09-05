#ifndef ELY_LAUNCHER_H
#define ELY_LAUNCHER_H

#include <gtk/gtk.h>
#include <gtk-layer-shell.h>
#include <gdk/gdk.h>
#include <cairo.h>
#include <pango/pango.h>
#include <gio/gio.h>
#include <iostream>
#include <string>
#include <vector>
#include <fstream>
#include <algorithm>
#include <dirent.h>
#include <cmath>
#include <unordered_set>
#include <set>
#include <map>
#include <sys/stat.h>
#include <thread>
#include <random>
#include <utility>
#include <unordered_map>
#include <cstdio>
#include <curl/curl.h>
#include <json/json.h>
#include <thread>

struct AppInfo {
    std::string name;
    std::string icon;
    std::string exec;
    std::string desktop_file;
    int usage_count = 0;
    time_t last_used = 0;
};

struct EmojiItem { 
    std::string glyph; 
    std::string keywords; 
};

struct GifItem {
    std::string url;
    std::string name;
    std::string preview_url;
    std::string tenor_id;
    bool thumbnail_loaded = false;
};

struct LauncherConfig {
    int emoji_size = 50;
    int gif_size = 64;
    bool emoji_enabled = true;
    bool gifs_enabled = true;
    bool files_enabled = true;
    std::string config_path;
};

// Glitter particles
struct GlitterParticle {
    double x;
    double y;
    double vx;
    double vy;
    double life_ms;
    double max_life_ms;
    double radius;
    GdkRGBA color;
};

// Theming
enum class ThemeVariant { ElysiaOS, ElysiaOS_HoC, Other };
enum class ViewMode { Apps, Emojis, Gifs, Files, Wallpapers };

// Forward declarations
class AppsManager;
class EmojiManager;
class GifManager;
class FilesManager;
class WallpaperManager;

class EnhancedEdgeLauncher {
private:
    GtkWidget* window;
    GtkWidget* layout;
    GtkWidget* bg_image;
    GtkWidget* search_entry;
    GtkWidget* suggestion_label;  // Gray text overlay for auto-completion
    GtkWidget* glitter_area;
    GtkWidget* app_name_label;
    GtkWidget* mode_apps_button;
    GtkWidget* mode_emojis_button;
    GtkWidget* mode_gifs_button;
    GtkWidget* mode_files_button;
    
    // Managers for different functionalities
    AppsManager* apps_manager;
    EmojiManager* emoji_manager;
    GifManager* gif_manager;
    FilesManager* files_manager;
    WallpaperManager* wallpaper_manager;
    
    // Glitter animation
    std::vector<GlitterParticle> glitter_particles;
    guint glitter_timer = 0;
    std::mt19937 rng{std::random_device{}()};
    
    // Animation
    double animation_progress = 0.0;
    guint animation_timer = 0;
    static constexpr int ANIMATION_DURATION_MS = 200;
    static constexpr int ANIMATION_FPS = 30;
    static constexpr int ANIMATION_INTERVAL_MS = 1000 / ANIMATION_FPS;
    
    // Theming
    ThemeVariant theme_variant = ThemeVariant::ElysiaOS;
    std::string accent_hex = "#FD84CB";    // default pink
    std::string border_hex = "#FD84CB";    // default pink border
    std::string image_base_dir = std::string(g_get_home_dir()) + "/.config/Elysia/assets/launcher";
    std::string image_filename = "elfely.png"; // default image name
    
    // Window properties
    int png_width = 1000;
    int window_width;
    gboolean buttons_visible = TRUE;
    ViewMode current_mode = ViewMode::Apps;
    int selected_index = -1; // index into current mode's filtered items
    
    // Auto-completion state
    std::string current_suggestion;
    std::string user_input;
    bool has_suggestion = false;
    
    // Background
    GdkPixbuf* bg_pixbuf = nullptr;
    GdkPixbuf* placeholder_pixbuf = nullptr;
    
    // Cache file management
    std::string cache_file_path;
    std::map<std::string, int> app_usage_cache;
    
    // Configuration
    LauncherConfig config;

public:
    EnhancedEdgeLauncher();
    ~EnhancedEdgeLauncher();
    
    void create_widget();
    void run();
    
    // Getters for managers
    AppsManager* get_apps_manager() const { return apps_manager; }
    EmojiManager* get_emoji_manager() const { return emoji_manager; }
    GifManager* get_gif_manager() const { return gif_manager; }
    FilesManager* get_files_manager() const { return files_manager; }
    WallpaperManager* get_wallpaper_manager() const { return wallpaper_manager; }
    
    // Getters for UI elements
    GtkWidget* get_window() const { return window; }
    GtkWidget* get_layout() const { return layout; }
    GtkWidget* get_search_entry() const { return search_entry; }
    GtkWidget* get_app_name_label() const { return app_name_label; }
    GtkWidget* get_mode_apps_button() const { return mode_apps_button; }
    GtkWidget* get_mode_emojis_button() const { return mode_emojis_button; }
    GtkWidget* get_mode_gifs_button() const { return mode_gifs_button; }
    GtkWidget* get_mode_files_button() const { return mode_files_button; }
    
    // Mode management
    ViewMode get_current_mode() const { return current_mode; }
    void set_current_mode(ViewMode mode) { current_mode = mode; }
    void switch_to_apps();
    void switch_to_emojis();
    void switch_to_gifs();
    void switch_to_files();
    void refresh_current_view();
    
    // Theming
    std::string get_accent_hex() const { return accent_hex; }
    std::string get_border_hex() const { return border_hex; }
    ThemeVariant get_theme_variant() const { return theme_variant; }
    
    // Cache management
    void increment_app_usage(const std::string& app_name);
    const std::map<std::string, int>& get_app_usage_cache() const { return app_usage_cache; }
    
    // Configuration management
    void load_config();
    void save_config();
    void create_default_config();
    void update_emoji_file();
    const LauncherConfig& get_config() const { return config; }
    bool validate_config();
    void backup_config();
    void restore_config();
    
    // Glitter animation
    void spawn_glitter_burst(int count);
    gboolean on_glitter_tick();
    
    // Animation
    void start_opening_animation();
    
    // Utility functions
    static std::string hex_to_rgba_060(const std::string& hex);
    void detect_theme_variant();
    void load_cache();
    void save_cache();
    void apply_css();
    void create_mode_buttons();
    
    // Static callbacks
    static gboolean delayed_quit_cb(gpointer);
    static gboolean animate_opening(gpointer data);
    static gboolean on_glitter_draw(GtkWidget* widget, cairo_t* cr, gpointer data);
    static void on_glitter_realize(GtkWidget* widget, gpointer data);
    static gboolean on_scroll_event(GtkWidget* widget, GdkEventScroll* event, gpointer data);
    static gboolean on_click_event(GtkWidget* widget, GdkEventButton* event, gpointer data);
    static gboolean on_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static gboolean on_search_key_press(GtkWidget* widget, GdkEventKey* event, gpointer data);
    static void on_search_changed(GtkEditable* editable, gpointer data);
    
    // Clipboard and quit
    void copy_to_clipboard_and_quit(const std::string& text);
    
    // Background loading
    void create_placeholder_background();
    void load_background_async();
    static gboolean apply_bg_pixbuf_idle(gpointer data);
    
    // Glitter helpers
    void ensure_glitter_timer_running();
    GdkRGBA pick_lesbian_palette_color();
    
    // UI helpers
    void update_app_name_label();
    
    // Auto-completion helpers
    void update_auto_completion(const std::string& query);
    std::string find_best_suggestion(const std::string& query);
    void show_suggestion_overlay(const std::string& user_input, const std::string& suggestion);
    void apply_suggestion();
    void clear_suggestion();
};

#endif // ELY_LAUNCHER_H 