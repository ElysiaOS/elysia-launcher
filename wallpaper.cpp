#include "ely_launcher.h"
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <thread>
#include <fstream>
#include <openssl/md5.h>
#include <iomanip>
#include <sstream>

struct WallpaperItem {
    std::string name;
    std::string path;
    std::string full_path;
    bool is_image = false;
};

class WallpaperManager {
private:
    EnhancedEdgeLauncher* launcher;
    std::vector<WallpaperItem> all_wallpapers;
    std::vector<WallpaperItem> filtered_wallpapers;
    std::vector<GtkWidget*> wallpaper_buttons;
    int selected_index = -1;
    size_t current_page = 0;
    static constexpr size_t WALLPAPERS_PER_PAGE = 12;
    bool wallpapers_loaded = false;
    std::string current_search_query;
    
    // Thumbnail cache
    std::unordered_map<std::string, GdkPixbuf*> thumbnail_cache;
    
    // Cache directory
    std::string cache_directory;
    
    // Image extensions
    static const std::set<std::string> image_extensions;
    
    // Create cache directory if it doesn't exist
    void create_cache_directory() {
        // Create base cache directory
        std::string base_cache_dir = std::string(g_get_user_cache_dir()) + "/ely_launcher";
        mkdir(base_cache_dir.c_str(), 0755);
        
        // Create thumbnails directory
        cache_directory = base_cache_dir + "/thumbnails";
        mkdir(cache_directory.c_str(), 0755);
    }
    
    // Generate MD5 hash for a string (used for filename hashing)
    std::string generate_md5(const std::string& input) {
        unsigned char digest[MD5_DIGEST_LENGTH];
        MD5(reinterpret_cast<const unsigned char*>(input.c_str()), input.length(), digest);
        
        std::ostringstream oss;
        for (int i = 0; i < MD5_DIGEST_LENGTH; i++) {
            oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(digest[i]);
        }
        return oss.str();
    }
    
    // Get file modification time
    time_t get_file_modification_time(const std::string& filepath) {
        struct stat attrib;
        if (stat(filepath.c_str(), &attrib) == 0) {
            return attrib.st_mtime;
        }
        return 0;
    }
    
    bool is_image_file(const std::string& path) {
        size_t dot_pos = path.find_last_of('.');
        if (dot_pos == std::string::npos) return false;
        
        std::string ext = path.substr(dot_pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return image_extensions.find(ext) != image_extensions.end();
    }
    
    std::string get_theme_directory() {
        // Check if we're in dark or light theme
        std::string theme_name = "";
        
        // Try GSettings first
        GSettings* settings = g_settings_new("org.gnome.desktop.interface");
        gchar* theme = nullptr;
        if (settings) {
            theme = g_settings_get_string(settings, "gtk-theme");
        }
        theme_name = theme ? std::string(theme) : "";
        if (theme) g_free(theme);
        if (settings) g_object_unref(settings);
        
        // Fallback to env GTK_THEME
        if (theme_name.empty()) {
            const char* env_theme = g_getenv("GTK_THEME");
            if (env_theme) theme_name = env_theme;
        }
        
        std::string lower = theme_name;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        
        // Determine if it's dark or light theme
        // Check for common dark theme indicators
        bool is_dark = false;
        if (lower.find("dark") != std::string::npos || 
            lower.find("night") != std::string::npos ||
            lower.find("black") != std::string::npos ||
            lower.find("dracula") != std::string::npos ||
            lower.find("gruvbox") != std::string::npos ||
            lower.find("nord") != std::string::npos ||
            lower.find("tokyo") != std::string::npos ||
            lower.find("catppuccin") != std::string::npos ||
            lower.find("ayu") != std::string::npos ||
            lower.find("solarized") != std::string::npos) {
            is_dark = true;
        }
        
        // Also check for color-scheme preference
        GSettings* color_scheme_settings = g_settings_new("org.gnome.desktop.interface");
        if (color_scheme_settings) {
            gchar* color_scheme = g_settings_get_string(color_scheme_settings, "color-scheme");
            if (color_scheme) {
                std::string scheme = color_scheme;
                if (scheme == "prefer-dark") {
                    is_dark = true;
                } else if (scheme == "prefer-light") {
                    is_dark = false;
                }
                g_free(color_scheme);
            }
            g_object_unref(color_scheme_settings);
        }
        
        std::string base_dir = std::string(g_get_home_dir()) + "/.config/Elysia/wallpaper/";
        return base_dir + (is_dark ? "Dark" : "Light");
    }
    
    void load_wallpapers_from_directory(const std::string& directory) {
        DIR* dir = opendir(directory.c_str());
        if (!dir) {
            printf("DEBUG: Could not open wallpaper directory: %s\n", directory.c_str());
            return;
        }
        
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) {
            if (entry->d_type == DT_REG) { // Regular file
                std::string filename = entry->d_name;
                std::string full_path = directory + "/" + filename;
                
                if (is_image_file(filename)) {
                    WallpaperItem item;
                    item.name = filename;
                    item.path = filename;
                    item.full_path = full_path;
                    item.is_image = true;
                    all_wallpapers.push_back(item);
                }
            }
        }
        closedir(dir);
    }
    
    GdkPixbuf* load_wallpaper_thumbnail(const std::string& path, int display_size = 60) {
        // Get file modification time
        time_t file_mod_time = get_file_modification_time(path);
        
        // Generate cache filename based on MD5 of path + modification time + size
        std::string path_hash = generate_md5(path + std::to_string(file_mod_time) + std::to_string(display_size));
        std::string cache_filename = cache_directory + "/" + path_hash + ".png";
        
        // Check if cached thumbnail exists
        struct stat cache_stat;
        if (stat(cache_filename.c_str(), &cache_stat) == 0) {
            // Check if cache file is still valid (created after the original file was last modified)
            if (cache_stat.st_mtime >= file_mod_time) {
                GdkPixbuf* cached_thumbnail = gdk_pixbuf_new_from_file(cache_filename.c_str(), nullptr);
                if (cached_thumbnail) {
                    return cached_thumbnail;
                }
            } else {
                // Cache is outdated, remove it
                unlink(cache_filename.c_str());
            }
        }
        
        // Check memory cache first with modification time in key
        std::string cache_key = path + "|" + std::to_string(file_mod_time) + "|" + std::to_string(display_size);
        auto it = thumbnail_cache.find(cache_key);
        if (it != thumbnail_cache.end() && it->second) {
            // Save to disk cache
            gdk_pixbuf_save(it->second, cache_filename.c_str(), "png", nullptr, nullptr);
            return GDK_PIXBUF(g_object_ref(it->second));
        }
        
        // Create new thumbnail
        GdkPixbuf* original = gdk_pixbuf_new_from_file(path.c_str(), nullptr);
        if (!original) {
            return nullptr;
        }
        
        // Create a square crop from the center of the image
        int orig_width = gdk_pixbuf_get_width(original);
        int orig_height = gdk_pixbuf_get_height(original);
        
        int crop_size = std::min(orig_width, orig_height);
        int crop_x = (orig_width - crop_size) / 2;
        int crop_y = (orig_height - crop_size) / 2;
        
        // Crop to square
        GdkPixbuf* cropped = gdk_pixbuf_new_subpixbuf(original, crop_x, crop_y, crop_size, crop_size);
        g_object_unref(original);
        
        if (!cropped) {
            return nullptr;
        }
        
        // Scale the cropped image to display size
        GdkPixbuf* display_thumbnail = gdk_pixbuf_scale_simple(cropped, display_size, display_size, GDK_INTERP_BILINEAR);
        g_object_unref(cropped);
        
        if (display_thumbnail) {
            // Cache in memory
            thumbnail_cache[cache_key] = GDK_PIXBUF(g_object_ref(display_thumbnail));
            
            // Save to disk cache
            gdk_pixbuf_save(display_thumbnail, cache_filename.c_str(), "png", nullptr, nullptr);
        }
        
        return display_thumbnail;
    }

public:
    WallpaperManager(EnhancedEdgeLauncher* l) : launcher(l) {
        create_cache_directory();
    }
    
    ~WallpaperManager() {
        for (auto* b : wallpaper_buttons) gtk_widget_destroy(b);
        
        // Clean up thumbnail cache
        for (auto& pair : thumbnail_cache) {
            if (pair.second) g_object_unref(pair.second);
        }
        thumbnail_cache.clear();
    }
    
    // Clear thumbnail cache to force refresh
    void clear_thumbnail_cache() {
        for (auto& pair : thumbnail_cache) {
            if (pair.second) g_object_unref(pair.second);
        }
        thumbnail_cache.clear();
    }
    
    void load_wallpapers() {
        if (wallpapers_loaded) {
            // Clear cache when reloading to ensure we get updated thumbnails
            clear_thumbnail_cache();
            return;
        }
        
        all_wallpapers.clear();
        std::string wallpaper_dir = get_theme_directory();
        load_wallpapers_from_directory(wallpaper_dir);
        
        wallpapers_loaded = true;
        filtered_wallpapers = all_wallpapers;
        current_page = 0;
        selected_index = filtered_wallpapers.empty() ? -1 : 0;
        
        printf("DEBUG: Loaded %zu wallpapers from %s\n", all_wallpapers.size(), wallpaper_dir.c_str());
    }
    
    void create_wallpaper_buttons() {
        // Clear existing buttons
        for (auto* b : wallpaper_buttons) gtk_widget_destroy(b);
        wallpaper_buttons.clear();
        
        if (filtered_wallpapers.empty()) return;
        
        size_t start_idx = current_page * WALLPAPERS_PER_PAGE;
        size_t end_idx = std::min(start_idx + WALLPAPERS_PER_PAGE, filtered_wallpapers.size());
        
        int button_x = 65;
        int button_y = 160;
        int buttons_per_row = 4;
        int button_spacing = 70;
        
        for (size_t i = start_idx; i < end_idx; ++i) {
            const auto& wallpaper = filtered_wallpapers[i];
            
            GtkWidget* btn = gtk_button_new();
            gtk_widget_set_name(btn, "wallpaper-button");
            gtk_widget_set_size_request(btn, 60, 60);
            
            // Load and set wallpaper thumbnail (cached at 250x250 but displayed at 60x60)
            GdkPixbuf* thumbnail = load_wallpaper_thumbnail(wallpaper.full_path, 60);
            if (thumbnail) {
                GtkWidget* image = gtk_image_new_from_pixbuf(thumbnail);
                gtk_button_set_image(GTK_BUTTON(btn), image);
                g_object_unref(thumbnail);
            } else {
                // Fallback to filename label if thumbnail loading fails
                std::string display_name = wallpaper.name;
                if (display_name.length() > 8) {
                    display_name = display_name.substr(0, 5) + "...";
                }
                GtkWidget* label = gtk_label_new(display_name.c_str());
                gtk_label_set_max_width_chars(GTK_LABEL(label), 8);
                gtk_container_add(GTK_CONTAINER(btn), label);
            }
            
            // Position button
            int row = (i - start_idx) / buttons_per_row;
            int col = (i - start_idx) % buttons_per_row;
            int x = button_x + col * button_spacing;
            int y = button_y + row * button_spacing;
            
            gtk_fixed_put(GTK_FIXED(launcher->get_layout()), btn, x, y);
            
            // Set tooltip with wallpaper name
            gtk_widget_set_tooltip_text(btn, wallpaper.name.c_str());
            
            // Connect click signal
            g_signal_connect(btn, "clicked", G_CALLBACK(on_wallpaper_button_clicked), &filtered_wallpapers[i]);
            
            // Store manager reference in button for callback access
            g_object_set_data(G_OBJECT(btn), "wallpaper_manager", this);
            
            gtk_widget_set_visible(btn, TRUE);
            gtk_widget_show(btn);
            wallpaper_buttons.push_back(btn);
        }
        
        update_selection_visuals();
    }
    
    void destroy_wallpaper_buttons() {
        for (auto* b : wallpaper_buttons) gtk_widget_destroy(b);
        wallpaper_buttons.clear();
    }
    
    void refresh_current_view() {
        clear_thumbnail_cache();
        create_wallpaper_buttons();
    }
    
    void update_app_name_label() {
        if (!launcher->get_app_name_label()) return;
        std::string text;
        if (selected_index >= 0 && selected_index < static_cast<int>(filtered_wallpapers.size())) {
            text = filtered_wallpapers[selected_index].name;
        } else if (!current_search_query.empty() && filtered_wallpapers.empty()) {
            text = "No wallpapers found for: " + current_search_query;
        } else if (!current_search_query.empty()) {
            text = "Found " + std::to_string(filtered_wallpapers.size()) + " wallpapers";
        } else {
            std::string theme_dir = get_theme_directory();
            bool is_dark = theme_dir.find("/Dark") != std::string::npos;
            text = "Wallpapers (" + std::string(is_dark ? "Dark" : "Light") + " theme) - " + std::to_string(all_wallpapers.size()) + " available";
        }
        gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), text.c_str());
    }
    
    void ensure_selection_initialized() {
        if (selected_index < 0 && !filtered_wallpapers.empty()) selected_index = 0;
        if (selected_index >= static_cast<int>(filtered_wallpapers.size()))
            selected_index = filtered_wallpapers.empty() ? -1 : static_cast<int>(filtered_wallpapers.size()) - 1;
    }
    
    void update_selection_visuals() {
        ensure_selection_initialized();
        
        for (size_t i = 0; i < wallpaper_buttons.size(); ++i) {
            GtkWidget* btn = wallpaper_buttons[i];
            size_t actual_index = current_page * WALLPAPERS_PER_PAGE + i;
            
            if (actual_index < filtered_wallpapers.size()) {
                if (static_cast<int>(actual_index) == selected_index) {
                    gtk_widget_set_name(btn, "wallpaper-button selected");
                } else {
                    gtk_widget_set_name(btn, "wallpaper-button");
                }
            }
        }
    }
    
    void select_prev() {
        if (filtered_wallpapers.empty()) return;
        
        if (selected_index > 0) {
            selected_index--;
        } else {
            selected_index = filtered_wallpapers.size() - 1;
        }
        
        // Check if we need to change page
        size_t items_per_page = WALLPAPERS_PER_PAGE;
        size_t new_page = selected_index / items_per_page;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
        
        update_app_name_label();
    }
    
    void select_next() {
        if (filtered_wallpapers.empty()) return;
        
        if (selected_index < static_cast<int>(filtered_wallpapers.size()) - 1) {
            selected_index++;
        } else {
            selected_index = 0;
        }
        
        // Check if we need to change page
        size_t items_per_page = WALLPAPERS_PER_PAGE;
        size_t new_page = selected_index / items_per_page;
        if (new_page != current_page) {
            current_page = new_page;
            refresh_current_view();
        } else {
            update_selection_visuals();
        }
        
        update_app_name_label();
    }
    
    void activate_selected() {
        if (selected_index >= 0 && selected_index < static_cast<int>(filtered_wallpapers.size())) {
            apply_wallpaper(selected_index);
        }
    }
    
    void scroll_up() {
        size_t total = filtered_wallpapers.size();
        if (total == 0) return;
        size_t max_page = (total - 1) / WALLPAPERS_PER_PAGE;
        if (current_page > 0) {
            current_page--;
            refresh_current_view();
        }
    }
    
    void scroll_down() {
        size_t total = filtered_wallpapers.size();
        if (total == 0) return;
        size_t max_page = (total - 1) / WALLPAPERS_PER_PAGE;
        if (current_page < max_page) {
            current_page++;
            refresh_current_view();
        }
    }
    
    void apply_wallpaper(size_t index) {
        if (index >= filtered_wallpapers.size()) return;
        
        const auto& wallpaper = filtered_wallpapers[index];
        std::string command = "swww img --transition-duration 2 --transition-type grow --transition-step 45 --transition-fps 40 \"" + wallpaper.full_path + "\"";
        
        printf("DEBUG: Applying wallpaper: %s\n", command.c_str());
        
        // Execute the command
        int result = system(command.c_str());
        if (result == 0) {
            printf("DEBUG: Wallpaper applied successfully\n");
            // Close the launcher after applying wallpaper
            g_idle_add([](gpointer) -> gboolean {
                gtk_main_quit();
                return G_SOURCE_REMOVE;
            }, nullptr);
        } else {
            printf("DEBUG: Failed to apply wallpaper, exit code: %d\n", result);
        }
    }
    
    void filter_wallpapers(const std::string& query) {
        current_search_query = query;
        
        if (query.empty()) {
            filtered_wallpapers = all_wallpapers;
        } else {
            filtered_wallpapers.clear();
            filtered_wallpapers.reserve(all_wallpapers.size());
            
            std::string lower_query = query;
            std::transform(lower_query.begin(), lower_query.end(), lower_query.begin(), ::tolower);
            
            for (const auto& wallpaper : all_wallpapers) {
                std::string lower_name = wallpaper.name;
                std::transform(lower_name.begin(), lower_name.end(), lower_name.begin(), ::tolower);
                
                if (lower_name.find(lower_query) != std::string::npos) {
                    filtered_wallpapers.push_back(wallpaper);
                }
            }
        }
        
        current_page = 0;
        selected_index = filtered_wallpapers.empty() ? -1 : 0;
        refresh_current_view();
        update_app_name_label();
    }
    
    void ensure_ready() {
        load_wallpapers();
    }
    
    // Getters
    const std::vector<WallpaperItem>& get_filtered_wallpapers() const { return filtered_wallpapers; }
    int get_selected_index() const { return selected_index; }
    size_t get_current_page() const { return current_page; }
    const std::vector<GtkWidget*>& get_wallpaper_buttons() const { return wallpaper_buttons; }
    
    // Static callback
    static void on_wallpaper_button_clicked(GtkWidget* widget, gpointer data) {
        WallpaperItem* wallpaper = static_cast<WallpaperItem*>(data);
        WallpaperManager* self = static_cast<WallpaperManager*>(
            g_object_get_data(G_OBJECT(widget), "wallpaper_manager"));
        
        if (self && wallpaper) {
            // Find the index of this wallpaper in the filtered list
            for (size_t i = 0; i < self->filtered_wallpapers.size(); ++i) {
                if (&self->filtered_wallpapers[i] == wallpaper) {
                    self->apply_wallpaper(i);
                    break;
                }
            }
        }
    }
};

// Define supported image extensions
const std::set<std::string> WallpaperManager::image_extensions = {
    "jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif",
    "webp", "svg", "ico", "xpm", "pbm", "pgm", "ppm"
}; 