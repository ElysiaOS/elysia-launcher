#include "ely_launcher.h"
#include <queue>
#include <deque>
#include <atomic>
#include <mutex>
#include <set>
#include <unordered_map>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <cctype>
#include <fstream>
#include <thread>
#include <dirent.h>
#include <pango/pango.h>

class FilesManager {
private:
	struct FileEntry {
		std::string path;
		std::string name;
		bool is_directory = false;
		bool is_image = false;
	};

	std::vector<FileEntry> filtered_entries;
	std::vector<GtkWidget*> file_buttons;
	EnhancedEdgeLauncher* launcher;
	size_t current_page = 0;
	static constexpr size_t FILES_PER_PAGE = 7;
	int selected_index = -1;
	guint search_timeout_id = 0;
	std::atomic<int> search_generation{0};
	std::atomic<bool> search_in_progress{false};
	std::mutex results_mutex;
	std::string last_query;

	GdkPixbuf* folder_icon = nullptr;
	GdkPixbuf* file_icon = nullptr;

	// Thumbnail cache
	std::unordered_map<std::string, GdkPixbuf*> thumbnail_cache;
	std::mutex thumbnail_mutex;
	static constexpr int THUMBNAIL_SIZE = 50;

	// Image file extensions
	static const std::set<std::string> image_extensions;

	// Special folder mappings
	static const std::unordered_map<std::string, std::string> special_folders;

	// ---------- helpers ----------
	static bool is_accessible_dir(const std::string& path) {
		struct stat st {};
		if (lstat(path.c_str(), &st) != 0) return false;
		if (!S_ISDIR(st.st_mode)) return false;
		// skip special mount points
		static const std::set<std::string> skip = {
			"/proc", "/sys", "/dev", "/run", "/tmp/.X11-unix", "/var/run"
		};
		if (skip.find(path) != skip.end()) return false;
		return access(path.c_str(), R_OK | X_OK) == 0;
	}

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
		std::string ext = get_file_extension(path);
		return image_extensions.find(ext) != image_extensions.end();
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
		// Check cache first
		{
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			auto it = thumbnail_cache.find(path);
			if (it != thumbnail_cache.end()) {
				return it->second ? g_object_ref(it->second) : nullptr;
			}
		}

		GError* error = nullptr;
		GdkPixbuf* original = gdk_pixbuf_new_from_file(path.c_str(), &error);

		if (error) {
			g_error_free(error);
			// Cache the failure
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			thumbnail_cache[path] = nullptr;
			return nullptr;
		}

		if (!original) {
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			thumbnail_cache[path] = nullptr;
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

		// Cache the thumbnail
		{
			std::lock_guard<std::mutex> lock(thumbnail_mutex);
			thumbnail_cache[path] = thumbnail ? g_object_ref(thumbnail) : nullptr;
		}

		return thumbnail;
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
		self->current_page = 0;
		self->selected_index = self->filtered_entries.empty() ? -1 : 0;
		self->refresh_current_view();
		self->update_app_name_label();
		delete pack;
		return G_SOURCE_REMOVE;
	}

	void start_search_thread(const std::string& query, int generation) {
		search_in_progress = true;
		std::thread([this, query, generation]() {
			std::vector<FileEntry> results;
			results.reserve(1981920);
			std::string ql = query;
			std::transform(ql.begin(), ql.end(), ql.begin(), ::tolower);

			// Roots to search
			std::vector<std::string> roots = {"/"};
			std::string home = g_get_home_dir();
			if (!home.empty()) roots.insert(roots.begin(), home);

			std::set<std::string> visited_dirs;
			std::deque<std::string> queue;
			for (const auto& r : roots) {
				if (is_accessible_dir(r)) queue.push_back(r);
			}
			const size_t HARD_RESULT_LIMIT = 60000;
			const size_t HARD_SCAN_LIMIT = 1000000;
			size_t scanned = 0;
			while (!queue.empty()) {
				if (generation != search_generation.load()) break;
				std::string dir = queue.front();
				queue.pop_front();
				if (!visited_dirs.insert(dir).second) continue;
				DIR* d = opendir(dir.c_str());
				if (!d) continue;
				struct dirent* ent;
				while ((ent = readdir(d)) != nullptr) {
					if (generation != search_generation.load()) { closedir(d); goto done; }
					const char* name = ent->d_name;
					if (!name) continue;
					if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0) continue;
					std::string path = dir;
					if (path.size() > 1 && path.back() != '/') path += "/";
					path += name;
					struct stat st{};
					if (lstat(path.c_str(), &st) != 0) continue;
					bool is_dir = S_ISDIR(st.st_mode);
					bool is_img = !is_dir && is_image_file(path);

					// match by name OR full path
					std::string nm = name;
					std::string nml = nm;
					std::transform(nml.begin(), nml.end(), nml.begin(), ::tolower);
					std::string pathl = path;
					std::transform(pathl.begin(), pathl.end(), pathl.begin(), ::tolower);
					if (nml.find(ql) != std::string::npos || pathl.find(ql) != std::string::npos) {
						results.push_back(FileEntry{path, nm, is_dir, is_img});
						if (results.size() >= HARD_RESULT_LIMIT) { /* don't add more results */ }
					}
					if (is_dir) {
						if (is_accessible_dir(path)) queue.push_back(path);
					}
					if (++scanned >= HARD_SCAN_LIMIT || results.size() >= HARD_RESULT_LIMIT) break;
				}
				closedir(d);
				if (scanned >= HARD_SCAN_LIMIT || results.size() >= HARD_RESULT_LIMIT) break;
			}
			done:
			// sort results by name (case-insensitive) then by full path to surface duplicates clearly
			std::sort(results.begin(), results.end(), [](const FileEntry& a, const FileEntry& b){
				std::string an = a.name, bn = b.name;
				std::transform(an.begin(), an.end(), an.begin(), ::tolower);
				std::transform(bn.begin(), bn.end(), bn.begin(), ::tolower);
				if (an != bn) return an < bn;
				return a.path < b.path;
			});
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
	FilesManager(EnhancedEdgeLauncher* l) : launcher(l) {}

	~FilesManager() {
		for (auto* b : file_buttons) gtk_widget_destroy(b);
		if (folder_icon) g_object_unref(folder_icon);
		if (file_icon) g_object_unref(file_icon);

		// Clean up thumbnail cache
		std::lock_guard<std::mutex> lock(thumbnail_mutex);
		for (auto& pair : thumbnail_cache) {
			if (pair.second) g_object_unref(pair.second);
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

		size_t start_idx = current_page * FILES_PER_PAGE;
		size_t end_idx = std::min(start_idx + FILES_PER_PAGE, filtered_entries.size());

		int start_y = 150;
		int button_spacing = 60;
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
			// Show just the file name here (full path should be shown only in your dedicated area)
			text = filtered_entries[selected_index].name;
		} else if (!last_query.empty() && filtered_entries.empty()) {
			text = "No files found";
		} else if (!last_query.empty()) {
			text = "Found " + std::to_string(filtered_entries.size()) + " items";
		} else {
			text = "Type to search files...";
		}
		gtk_label_set_ellipsize(GTK_LABEL(launcher->get_app_name_label()), PANGO_ELLIPSIZE_MIDDLE);
		gtk_label_set_max_width_chars(GTK_LABEL(launcher->get_app_name_label()), 40);
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
		size_t page_start = current_page * FILES_PER_PAGE;
		if (selected_index >= static_cast<int>(page_start) &&
		    selected_index < static_cast<int>(page_start + file_buttons.size())) {
			size_t local = static_cast<size_t>(selected_index) - page_start;
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
		size_t new_page = static_cast<size_t>(selected_index) / FILES_PER_PAGE;
		if (new_page != current_page) {
			current_page = new_page;
			refresh_current_view();
		} else {
			update_selection_visuals();
		}
	}

	void select_prev() {
		if (filtered_entries.empty()) return;
		ensure_selection_initialized();
		selected_index = std::max(selected_index - 1, 0);
		size_t new_page = static_cast<size_t>(selected_index) / FILES_PER_PAGE;
		if (new_page != current_page) {
			current_page = new_page;
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
		if (current_page == 0) return;
		current_page--;
		refresh_current_view();
	}

	void scroll_down() {
		size_t total = filtered_entries.size();
		if (total == 0) return;
		size_t max_page = (total - 1) / FILES_PER_PAGE;
		if (current_page < max_page) {
			current_page++;
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
			current_page = 0;
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
		search_timeout_id = g_timeout_add(
			250, debounced_search,
			new std::pair<FilesManager*, std::string>(this, query)
		);
	}

	// Getters
	const std::vector<FileEntry>& get_filtered_entries() const { return filtered_entries; }
	int get_selected_index() const { return selected_index; }
	size_t get_current_page() const { return current_page; }
	const std::vector<GtkWidget*>& get_file_buttons() const { return file_buttons; }
};

// Define supported image extensions
const std::set<std::string> FilesManager::image_extensions = {
	"jpg", "jpeg", "png", "gif", "bmp", "tiff", "tif",
	"webp", "svg", "ico", "xpm", "pbm", "pgm", "ppm"
};

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
