#include "ely_launcher.h"
#include <queue>
#include <deque>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_set>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <thread>
#include <dirent.h>
#include <pango/pango.h>

// Ultra-fast directory scanner with smart limits
class FastFileScanner {
public:
    struct FileEntry {
        std::string path;
        std::string name;
        bool is_directory;
        bool is_image;
        int score = 0;
    };

    static std::vector<FileEntry> search_files(const std::string& query) {
        if (query.empty()) return {};
        
        std::vector<FileEntry> results;
        results.reserve(100000); // Pre-allocate more space for unlimited results
        
        std::string query_lower = query;
        std::transform(query_lower.begin(), query_lower.end(), query_lower.begin(), ::tolower);
        
        // Smart search paths - start with most relevant locations
        std::vector<std::string> search_paths;
        std::string home = g_get_home_dir();
        if (!home.empty()) {
            search_paths = {
                home + "/Desktop",
                home + "/Documents", 
                home + "/Downloads",
                home + "/Pictures",
                home + "/Videos",
                home + "/Music",
                home,
                "/usr/share/applications",
                "/opt",
                "/usr/bin",
                "/"
            };
        } else {
            search_paths = {"/usr/share/applications", "/opt", "/usr/bin", "/"};
        }
        
        size_t found_count = 0;
        // NO LIMITS! Find everything!
        
        for (const auto& root : search_paths) {
            scan_directory_fast(root, query_lower, results, found_count, 0, 8); // Increased depth to find more files
        }
        
        // Sort by relevance score
        std::sort(results.begin(), results.end(), [](const FileEntry& a, const FileEntry& b) {
            if (a.score != b.score) return a.score > b.score;
            return a.name < b.name;
        });
        
        return results;
    }

private:
    static void scan_directory_fast(const std::string& dir_path, const std::string& query_lower, 
                                   std::vector<FileEntry>& results, size_t& found_count, 
                                   int depth, int max_depth) {
        if (depth > max_depth) return;
        if (!is_accessible_dir(dir_path)) return;
        
        DIR* dir = opendir(dir_path.c_str());
        if (!dir) return;
        
        struct dirent* entry;
        while ((entry = readdir(dir)) != nullptr) { // NO LIMITS HERE!
            if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
            
            std::string name = entry->d_name;
            std::string full_path = dir_path;
            if (full_path.back() != '/') full_path += "/";
            full_path += name;
            
            struct stat st;
            if (lstat(full_path.c_str(), &st) != 0) continue;
            
            bool is_dir = S_ISDIR(st.st_mode);
            bool is_img = !is_dir && is_image_file(name);
            
            // Fast string matching
            int score = calculate_match_score(query_lower, name, full_path);
            if (score > 0) {
                results.push_back({full_path, name, is_dir, is_img, score});
                found_count++;
            }
            
            // Recurse into directories (with depth limit only for performance)
            if (is_dir && depth < max_depth && is_worth_scanning(name)) {
                scan_directory_fast(full_path, query_lower, results, found_count, depth + 1, max_depth);
            }
        }
        closedir(dir);
    }
    
    static int calculate_match_score(const std::string& query, const std::string& name, const std::string& path) {
        std::string name_lower = name;
        std::transform(name_lower.begin(), name_lower.end(), name_lower.begin(), ::tolower);
        
        // Exact match gets highest score
        if (name_lower == query) return 1000;
        
        // Starts with query gets high score
        if (name_lower.find(query) == 0) return 500;
        
        // Contains query gets medium score
        if (name_lower.find(query) != std::string::npos) return 100;
        
        // Check path for matches
        std::string path_lower = path;
        std::transform(path_lower.begin(), path_lower.end(), path_lower.begin(), ::tolower);
        if (path_lower.find(query) != std::string::npos) return 50;
        
        // Fuzzy match for individual characters
        if (fuzzy_match(query, name_lower)) return 25;
        
        return 0;
    }
    
    static bool fuzzy_match(const std::string& query, const std::string& text) {
        size_t query_idx = 0;
        for (size_t i = 0; i < text.size() && query_idx < query.size(); ++i) {
            if (text[i] == query[query_idx]) {
                query_idx++;
            }
        }
        return query_idx == query.size();
    }
    
    static bool is_accessible_dir(const std::string& path) {
        struct stat st;
        if (lstat(path.c_str(), &st) != 0) return false;
        if (!S_ISDIR(st.st_mode)) return false;
        
        // Skip problematic directories
        static const std::unordered_set<std::string> skip_dirs = {
            "/proc", "/sys", "/dev", "/run", "/tmp/.X11-unix", "/var/run", "/snap"
        };
        return skip_dirs.find(path) == skip_dirs.end() && access(path.c_str(), R_OK | X_OK) == 0;
    }
    
    static bool is_worth_scanning(const std::string& name) {
        // Skip hidden directories and common uninteresting ones
        if (name[0] == '.') return false;
        static const std::unordered_set<std::string> skip = {
            "__pycache__", "node_modules", ".git", ".svn", "cache", "Cache"
        };
        return skip.find(name) == skip.end();
    }
    
    static bool is_image_file(const std::string& filename) {
        static const std::unordered_set<std::string> image_exts = {
            "jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif",
            "webp", "svg", "ico", "xpm", "pbm", "pgm", "ppm"
        };
        
        size_t pos = filename.find_last_of('.');
        if (pos == std::string::npos) return false;
        
        std::string ext = filename.substr(pos + 1);
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        return image_exts.count(ext) > 0;
    }
};

class FilesManager {
private:
	struct FileEntry {
		std::string path;
		std::string name;
		bool is_directory = false;
		bool is_image = false;
		int score = 0;
	};

	std::vector<FileEntry> filtered_entries;
	std::vector<GtkWidget*> file_buttons;
	EnhancedEdgeLauncher* launcher;
	size_t visible_start = 0;
	size_t visible_count = 7; // Maximum 7 items per page
	int selected_index = -1;
	guint search_timeout_id = 0;
	std::atomic<int> search_generation{0};
	std::atomic<bool> search_in_progress{false};
	std::mutex results_mutex;
	std::string last_query;

	GdkPixbuf* folder_icon = nullptr;
	GdkPixbuf* file_icon = nullptr;

	// Optimized thumbnail cache with LRU eviction
	mutable std::mutex thumbnail_mutex;
	std::unordered_map<std::string, std::pair<GdkPixbuf*, std::chrono::steady_clock::time_point>> thumbnail_cache;
	static constexpr int THUMBNAIL_SIZE = 50;
	static constexpr size_t MAX_THUMBNAIL_CACHE = 10000;

	// Special folder mappings (optimized)
	static const std::unordered_map<std::string, std::string> special_folders;

	static std::string basename_of(const std::string& path) {
		size_t pos = path.find_last_of('/');
		if (pos == std::string::npos) return path;
		if (pos == path.size() - 1) {
			// strip trailing slashes
			size_t start = path.find_last_not_of('/');
			if (start == std::string::npos) return "/";
			size_t prev = path.find_last_of('/', start);
			return path.substr(prev == std::string::npos ? 0 : prev + 1,
			                   start - (prev == std::string::npos ? 0 : prev + 1) + 1);
		}
		return path.substr(pos + 1);
	}

	static std::string get_file_extension(const std::string& filename) {
		size_t pos = filename.find_last_of('.');
		if (pos == std::string::npos) return "";
		std::string ext = filename.substr(pos + 1);
		std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
		return ext;
	}

	static bool is_image_file(const std::string& path) {
		static const std::unordered_set<std::string> image_exts = {
			"jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif",
			"webp", "svg", "ico", "xpm", "pbm", "pgm", "ppm"
		};
		
		std::string ext = get_file_extension(path);
		return image_exts.count(ext) > 0;
	}

	static std::string get_folder_icon_name(const std::string& path) {
		std::string basename = basename_of(path);
		std::string lower_basename = basename;
		std::transform(lower_basename.begin(), lower_basename.end(), lower_basename.begin(), ::tolower);

		// Check for special folders
		auto it = special_folders.find(lower_basename);
		if (it != special_folders.end()) {
			return it->second;
		}

		// Check if it's a user directory
		std::string home = g_get_home_dir();
		if (!home.empty() && path.find(home) == 0) {
			if (path == home) return "user-home";
			if (path == home + "/Desktop") return "user-desktop";
			if (path == home + "/Documents") return "folder-documents";
			if (path == home + "/Downloads") return "folder-download";
			if (path == home + "/Music") return "folder-music";
			if (path == home + "/Pictures") return "folder-pictures";
			if (path == home + "/Videos") return "folder-videos";
			if (path == home + "/Public") return "folder-publicshare";
			if (path == home + "/Templates") return "folder-templates";
		}

		return "folder";
	}

	static std::string parse_desktop_file_icon(const std::string& path) {
		std::ifstream file(path);
		if (!file.is_open()) return "";

		std::string line;
		bool in_desktop_entry = false;

		while (std::getline(file, line)) {
			// Trim whitespace
			line.erase(0, line.find_first_not_of(" \t"));
			line.erase(line.find_last_not_of(" \t") + 1);

			if (line.empty() || line[0] == '#') continue;

			if (line == "[Desktop Entry]") {
				in_desktop_entry = true;
				continue;
			}

			if (line[0] == '[') {
				in_desktop_entry = false;
				continue;
			}

			if (in_desktop_entry && line.find("Icon=") == 0) {
				std::string icon = line.substr(5);
				if (!icon.empty()) return icon;
			}
		}

		return "";
	}

	// Always return a pixbuf scaled to EXACTLY THUMBNAIL_SIZE x THUMBNAIL_SIZE
	GdkPixbuf* load_icon_from_theme(const std::string& icon_name, int size) {
		GtkIconTheme* icon_theme = gtk_icon_theme_get_default();
		GError* error = nullptr;

		GdkPixbuf* pixbuf = gtk_icon_theme_load_icon(
			icon_theme, icon_name.c_str(), size,
			GTK_ICON_LOOKUP_USE_BUILTIN, &error
		);

		if (error) {
			g_error_free(error);
			return nullptr;
		}

		if (!pixbuf) return nullptr;

		// Force exact sizing to avoid oversized .desktop icons
		GdkPixbuf* scaled = gdk_pixbuf_scale_simple(
			pixbuf, THUMBNAIL_SIZE, THUMBNAIL_SIZE, GDK_INTERP_BILINEAR
		);
		g_object_unref(pixbuf);
		return scaled;
	}

	void ensure_icons_loaded() {
		if (folder_icon && file_icon) return;
		folder_icon = load_icon_from_theme("folder", THUMBNAIL_SIZE);
		file_icon   = load_icon_from_theme("text-x-generic", THUMBNAIL_SIZE);
	}

	GdkPixbuf* load_image_thumbnail(const std::string& path) {
		auto now = std::chrono::steady_clock::now();
		
		// Check cache first with LRU updates
		{
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			auto it = thumbnail_cache.find(path);
			if (it != thumbnail_cache.end()) {
				it->second.second = now; // Update access time
				return it->second.first ? g_object_ref(it->second.first) : nullptr;
			}
		}

		GError* error = nullptr;
		GdkPixbuf* original = gdk_pixbuf_new_from_file(path.c_str(), &error);

		if (error) {
			g_error_free(error);
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			evict_old_thumbnails();
			thumbnail_cache[path] = {nullptr, now};
			return nullptr;
		}

		if (!original) {
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			evict_old_thumbnails();
			thumbnail_cache[path] = {nullptr, now};
			return nullptr;
		}

		// Create thumbnail (preserve aspect ratio for user images)
		int orig_width = gdk_pixbuf_get_width(original);
		int orig_height = gdk_pixbuf_get_height(original);

		int thumb_width, thumb_height;
		if (orig_width > orig_height) {
			thumb_width = THUMBNAIL_SIZE;
			thumb_height = (orig_height * THUMBNAIL_SIZE) / orig_width;
		} else {
			thumb_height = THUMBNAIL_SIZE;
			thumb_width = (orig_width * THUMBNAIL_SIZE) / orig_height;
		}

		// Ensure minimum size
		if (thumb_width < 1) thumb_width = 1;
		if (thumb_height < 1) thumb_height = 1;

		GdkPixbuf* thumbnail = gdk_pixbuf_scale_simple(
			original, thumb_width, thumb_height, GDK_INTERP_BILINEAR
		);
		g_object_unref(original);

		// Cache the thumbnail with LRU eviction
		{
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			evict_old_thumbnails();
			thumbnail_cache[path] = {thumbnail ? g_object_ref(thumbnail) : nullptr, now};
		}

		return thumbnail;
	}
	
	void evict_old_thumbnails() {
		if (thumbnail_cache.size() <= MAX_THUMBNAIL_CACHE) return;
		
		// Find oldest entries to evict
		std::vector<std::pair<std::string, std::chrono::steady_clock::time_point>> entries;
		for (const auto& pair : thumbnail_cache) {
			entries.emplace_back(pair.first, pair.second.second);
		}
		
		std::sort(entries.begin(), entries.end(), [](const auto& a, const auto& b) {
			return a.second < b.second;
		});
		
		// Evict oldest 25% of entries
		size_t to_evict = MAX_THUMBNAIL_CACHE / 4;
		for (size_t i = 0; i < to_evict && i < entries.size(); ++i) {
			auto it = thumbnail_cache.find(entries[i].first);
			if (it != thumbnail_cache.end()) {
				if (it->second.first) g_object_unref(it->second.first);
				thumbnail_cache.erase(it);
			}
		}
	}

	GdkPixbuf* get_file_icon(const FileEntry& entry) {
		if (entry.is_directory) {
			// Get appropriate folder icon (already exact-sized)
			std::string folder_icon_name = get_folder_icon_name(entry.path);
			GdkPixbuf* themed_icon = load_icon_from_theme(folder_icon_name, THUMBNAIL_SIZE);
			if (themed_icon) return themed_icon;
			return folder_icon ? g_object_ref(folder_icon) : nullptr;
		}

		// Handle .desktop files
		std::string ext = get_file_extension(entry.path);
		if (ext == "desktop") {
			std::string desktop_icon = parse_desktop_file_icon(entry.path);
			if (!desktop_icon.empty()) {
				// Try to load the desktop file's icon from theme (exact-sized)
				GdkPixbuf* app_icon = load_icon_from_theme(desktop_icon, THUMBNAIL_SIZE);
				if (app_icon) return app_icon;

				// If icon name has a path, try to load directly at exact size
				if (desktop_icon.find('/') != std::string::npos) {
					GError* error = nullptr;
					GdkPixbuf* direct_icon = gdk_pixbuf_new_from_file_at_scale(
						desktop_icon.c_str(), THUMBNAIL_SIZE, THUMBNAIL_SIZE, TRUE, &error
					);
					if (error) {
						g_error_free(error);
					} else if (direct_icon) {
						// If aspect preserved smaller than box, upscale to exact to normalize
						if (gdk_pixbuf_get_width(direct_icon) != THUMBNAIL_SIZE ||
						    gdk_pixbuf_get_height(direct_icon) != THUMBNAIL_SIZE) {
							GdkPixbuf* exact = gdk_pixbuf_scale_simple(
								direct_icon, THUMBNAIL_SIZE, THUMBNAIL_SIZE, GDK_INTERP_BILINEAR
							);
							g_object_unref(direct_icon);
							return exact;
						}
						return direct_icon;
					}
				}
			}

			// Fallback to application icon (exact-sized)
			GdkPixbuf* app_fallback = load_icon_from_theme("application-x-executable", THUMBNAIL_SIZE);
			if (app_fallback) return app_fallback;
		}

		// Handle image files (keep aspect ratio)
		if (entry.is_image) {
			GdkPixbuf* thumb = load_image_thumbnail(entry.path);
			if (thumb) return thumb;
		}

		// Try to get file type specific icon based on extension (exact-sized via load_icon_from_theme)
		if (!ext.empty()) {
			std::string mime_icon = "text-x-" + ext;
			GdkPixbuf* ext_icon = load_icon_from_theme(mime_icon, THUMBNAIL_SIZE);
			if (ext_icon) return ext_icon;

			// Try some common file type mappings
			static const std::unordered_map<std::string, std::string> file_type_icons = {
				{"pdf", "application-pdf"},
				{"doc", "application-msword"}, {"docx", "application-msword"},
				{"xls", "application-vnd.ms-excel"}, {"xlsx", "application-vnd.ms-excel"},
				{"ppt", "application-vnd.ms-powerpoint"}, {"pptx", "application-vnd.ms-powerpoint"},
				{"zip", "application-zip"}, {"rar", "application-zip"}, {"7z", "application-zip"},
				{"tar", "application-x-tar"}, {"gz", "application-x-tar"},
				{"mp3", "audio-x-generic"}, {"wav", "audio-x-generic"}, {"flac", "audio-x-generic"},
				{"mp4", "video-x-generic"}, {"avi", "video-x-generic"}, {"mkv", "video-x-generic"},
				{"txt", "text-x-generic"}, {"log", "text-x-generic"},
				{"html", "text-html"}, {"xml", "text-xml"},
				{"py", "text-x-python"}, {"cpp", "text-x-c++src"}, {"c", "text-x-csrc"},
				{"js", "text-x-javascript"}, {"css", "text-css"}
			};

			auto it = file_type_icons.find(ext);
			if (it != file_type_icons.end()) {
				GdkPixbuf* type_icon = load_icon_from_theme(it->second, THUMBNAIL_SIZE);
				if (type_icon) return type_icon;
			}
		}

		// Fallback to generic file icon (already exact-sized)
		return file_icon ? g_object_ref(file_icon) : nullptr;
	}

	static gboolean apply_results_idle(gpointer data) {
		auto* pack = static_cast<std::pair<FilesManager*, std::vector<FileEntry>>*>(data);
		FilesManager* self = pack->first;
		{
			std::lock_guard<std::mutex> lk(self->results_mutex);
			self->filtered_entries = std::move(pack->second);
		}
		self->visible_start = 0;
		self->selected_index = self->filtered_entries.empty() ? -1 : 0;
		self->refresh_current_view();
		self->update_app_name_label();
		delete pack;
		return G_SOURCE_REMOVE;
	}

	void start_search_thread(const std::string& query, int generation) {
		search_in_progress = true;
		std::thread([this, query, generation]() {
			// Use the ultra-fast on-demand scanner - NO INDEXING NEEDED!
			auto scanner_results = FastFileScanner::search_files(query);
			
			std::vector<FileEntry> results;
			results.reserve(scanner_results.size());
			
			for (const auto& scanner_entry : scanner_results) {
				if (generation != search_generation.load()) break;
				
				results.push_back(FileEntry{
					scanner_entry.path,
					scanner_entry.name,
					scanner_entry.is_directory,
					scanner_entry.is_image,
					scanner_entry.score
				});
			}
			
			search_in_progress = false;
			if (generation == search_generation.load()) {
				g_idle_add(apply_results_idle, new std::pair<FilesManager*, std::vector<FileEntry>>(this, std::move(results)));
			}
		}).detach();
	}

	static gboolean debounced_search(gpointer data) {
		auto* p = static_cast<std::pair<FilesManager*, std::string>*>(data);
		FilesManager* self = p->first;
		std::string query = p->second;
		delete p;
		self->perform_search(query);
		self->search_timeout_id = 0;
		return G_SOURCE_REMOVE;
	}

public:
	FilesManager(EnhancedEdgeLauncher* l) : launcher(l) {
		// No more background indexing - pure on-demand search!
	}

	~FilesManager() {
		for (auto* b : file_buttons) gtk_widget_destroy(b);
		if (folder_icon) g_object_unref(folder_icon);
		if (file_icon) g_object_unref(file_icon);

		// Clean up optimized thumbnail cache
		std::lock_guard<std::mutex> lock(thumbnail_mutex);
		for (auto& pair : thumbnail_cache) {
			if (pair.second.first) g_object_unref(pair.second.first);
		}
		thumbnail_cache.clear();

		if (search_timeout_id > 0) g_source_remove(search_timeout_id);
	}

	void ensure_ready() {
		ensure_icons_loaded();
		// Prevent header label from stretching layout on long names
		if (launcher && launcher->get_app_name_label()) {
			GtkWidget* lbl = launcher->get_app_name_label();
			gtk_label_set_ellipsize(GTK_LABEL(lbl), PANGO_ELLIPSIZE_MIDDLE);
			gtk_label_set_max_width_chars(GTK_LABEL(lbl), 40); // tune as needed
		}
	}

	void create_file_buttons() {
		// Clear existing buttons
		for (auto* button : file_buttons) {
			gtk_widget_destroy(button);
		}
		file_buttons.clear();

		// Virtual scrolling - show only visible items, NO HARD LIMITS!
		size_t start_idx = visible_start;
		size_t end_idx = std::min(visible_start + visible_count, filtered_entries.size());

		int start_y = 150;
		int button_spacing = 50; // Slightly tighter spacing for more items
		int button_x = 85;

		ensure_icons_loaded();

		for (size_t i = start_idx; i < end_idx; ++i) {
			GtkWidget* button = gtk_button_new();
			gtk_widget_set_name(button, "app-button");
			gtk_widget_set_size_request(button, THUMBNAIL_SIZE + 8, THUMBNAIL_SIZE + 8);

			// Get appropriate icon/thumbnail (icons exact size, images aspect-fitted)
			GdkPixbuf* pix = get_file_icon(filtered_entries[i]);
			if (pix) {
				GtkWidget* icon = gtk_image_new_from_pixbuf(pix);
				// Make sure the image widget space is fixed so icons are uniform
				gtk_widget_set_size_request(icon, THUMBNAIL_SIZE, THUMBNAIL_SIZE);
				gtk_button_set_image(GTK_BUTTON(button), icon);
				g_object_unref(pix); // Release our reference
			}

			// Show full path on hover as requested
			gtk_widget_set_tooltip_text(button, filtered_entries[i].path.c_str());

			g_signal_connect(
				button, "clicked",
				G_CALLBACK(+[](GtkWidget* btn, gpointer data){
					auto* entry = static_cast<FileEntry*>(data);
					if (!entry) return;
					std::string cmd = std::string("xdg-open ") + "\"" + entry->path + "\" &";
					system(cmd.c_str());
					gtk_main_quit();
				}),
				&filtered_entries[i]
			);
			g_object_set_data(G_OBJECT(button), "launcher", launcher);
			g_object_set_data_full(G_OBJECT(button), "file_name",
			                       g_strdup(filtered_entries[i].name.c_str()), g_free);

			int y_pos = start_y + (i - start_idx) * button_spacing;
			gtk_fixed_put(GTK_FIXED(launcher->get_layout()), button, button_x, y_pos);
			gtk_widget_set_visible(button, TRUE);
			file_buttons.push_back(button);
		}
		update_selection_visuals();
	}

	void destroy_file_buttons() {
		for (auto* b : file_buttons) gtk_widget_destroy(b);
		file_buttons.clear();
	}

	void refresh_current_view() {
		create_file_buttons();
	}

	void update_app_name_label() {
		if (!launcher->get_app_name_label()) return;
		std::string text;
		if (selected_index >= 0 && selected_index < static_cast<int>(filtered_entries.size())) {
			// Show just the file name here
			text = filtered_entries[selected_index].name;
		} else if (!last_query.empty() && filtered_entries.empty()) {
			if (search_in_progress) {
				text = "⚡ Searching...";
			} else {
				text = "No files found";
			}
		} else if (!last_query.empty()) {
			text = "Found " + std::to_string(filtered_entries.size()) + " items";
		} else {
			text = "Type to search files!";
		}
		gtk_label_set_ellipsize(GTK_LABEL(launcher->get_app_name_label()), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(launcher->get_app_name_label()), 45);
		gtk_label_set_text(GTK_LABEL(launcher->get_app_name_label()), text.c_str());
	}

	void ensure_selection_initialized() {
		if (selected_index < 0 && !filtered_entries.empty()) selected_index = 0;
		if (selected_index >= static_cast<int>(filtered_entries.size()))
			selected_index = filtered_entries.empty() ? -1 : static_cast<int>(filtered_entries.size()) - 1;
	}

	void update_selection_visuals() {
		ensure_selection_initialized();
		for (auto* b : file_buttons) {
			GtkStyleContext* ctx = gtk_widget_get_style_context(b);
			gtk_style_context_remove_class(ctx, "selected");
		}
		if (selected_index < 0) { update_app_name_label(); return; }
		
		// Check if selected item is in visible range
		if (selected_index >= static_cast<int>(visible_start) &&
		    selected_index < static_cast<int>(visible_start + file_buttons.size())) {
			size_t local = static_cast<size_t>(selected_index) - visible_start;
			if (local < file_buttons.size()) {
				GtkStyleContext* ctx = gtk_widget_get_style_context(file_buttons[local]);
				gtk_style_context_add_class(ctx, "selected");
			}
		}
		update_app_name_label();
	}

	void select_next() {
		if (filtered_entries.empty()) return;
		ensure_selection_initialized();
		int max_index = static_cast<int>(filtered_entries.size()) - 1;
		selected_index = std::min(selected_index + 1, max_index);
		
		// Update virtual scrolling view if selection is outside visible range
		if (selected_index >= static_cast<int>(visible_start + visible_count)) {
			visible_start = static_cast<size_t>(selected_index) - visible_count + 1;
			refresh_current_view();
		} else if (selected_index < static_cast<int>(visible_start)) {
			visible_start = static_cast<size_t>(selected_index);
			refresh_current_view();
		} else {
			update_selection_visuals();
		}
	}

	void select_prev() {
		if (filtered_entries.empty()) return;
		ensure_selection_initialized();
		selected_index = std::max(selected_index - 1, 0);
		
		// Update virtual scrolling view if selection is outside visible range
		if (selected_index < static_cast<int>(visible_start)) {
			visible_start = static_cast<size_t>(selected_index);
			refresh_current_view();
		} else if (selected_index >= static_cast<int>(visible_start + visible_count)) {
			visible_start = static_cast<size_t>(selected_index) - visible_count + 1;
			refresh_current_view();
		} else {
			update_selection_visuals();
		}
	}

	void activate_selected() {
		if (selected_index < 0 || selected_index >= static_cast<int>(filtered_entries.size())) return;
		const auto& e = filtered_entries[selected_index];
		std::string cmd = std::string("xdg-open ") + "\"" + e.path + "\" &";
		system(cmd.c_str());
		gtk_main_quit();
	}

	void scroll_up() {
		if (visible_start == 0) return;
		visible_start = (visible_start >= visible_count) ? visible_start - visible_count : 0;
		refresh_current_view();
	}

	void scroll_down() {
		size_t total = filtered_entries.size();
		if (total == 0) return;
		if (visible_start + visible_count < total) {
			visible_start += visible_count;
			if (visible_start >= total) {
				visible_start = (total > visible_count) ? total - visible_count : 0;
			}
			refresh_current_view();
		}
	}

	void perform_search(const std::string& query) {
		std::string trimmed = query;
		trimmed.erase(0, trimmed.find_first_not_of(" \t"));
		trimmed.erase(trimmed.find_last_not_of(" \t") + 1);
		last_query = trimmed;
		if (trimmed.length() < 1) {
			filtered_entries.clear();
			visible_start = 0;
			selected_index = -1;
			refresh_current_view();
			update_app_name_label();
			return;
		}
		int gen = ++search_generation;
		start_search_thread(trimmed, gen);
	}

	void filter_files(const std::string& query) {
		if (search_timeout_id > 0) {
			g_source_remove(search_timeout_id);
			search_timeout_id = 0;
		}
		// Ultra-fast search with minimal delay
		search_timeout_id = g_timeout_add(
			100, debounced_search, // Just enough to avoid excessive searches while typing
			new std::pair<FilesManager*, std::string>(this, query)
		);
	}

	// Getters
	const std::vector<FileEntry>& get_filtered_entries() const { return filtered_entries; }
	int get_selected_index() const { return selected_index; }
	size_t get_visible_start() const { return visible_start; }
	size_t get_visible_count() const { return visible_count; }
	const std::vector<GtkWidget*>& get_file_buttons() const { return file_buttons; }
};

// Removed image_extensions - now handled in FileSystemCache class

// Define special folder mappings
const std::unordered_map<std::string, std::string> FilesManager::special_folders = {
	{"desktop", "user-desktop"},
	{"documents", "folder-documents"},
	{"downloads", "folder-download"},
	{"music", "folder-music"},
	{"pictures", "folder-pictures"},
	{"videos", "folder-videos"},
	{"public", "folder-publicshare"},
	{"templates", "folder-templates"},
	{"trash", "user-trash"},
	{"bin", "folder"},
	{"etc", "folder-system"},
	{"usr", "folder-system"},
	{"var", "folder-system"},
	{"opt", "folder-system"},
	{"home", "user-home"},
	{"root", "folder-root"}
};
