#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <stdio.h>
#include <string>
#include <vector>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <thread>
#include <mutex>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <GLFW/glfw3.h>

// Linux-specific headers for inotify
#include <sys/inotify.h>
#include <sys/select.h>
#include <unistd.h>
#include <limits.h>
#include <errno.h>
#include <fcntl.h>



#include <sys/stat.h>



namespace fs = std::filesystem;

// Size of the buffer to store inotify events
#define EVENT_BUF_LEN (10 * (sizeof(struct inotify_event) + NAME_MAX + 1))

// File state enum to track different states
enum class FileState {
    Created,    // File was just created
    Modified,   // File was modified
    Accessed,   // File was accessed (read)
    Deleted     // File was deleted (for tracking)
};

// Structure to hold file information
struct FileInfo {
    std::chrono::system_clock::time_point timestamp;
    FileState state;
    std::chrono::system_clock::time_point stateChangeTime;

    FileInfo(std::chrono::system_clock::time_point time = std::chrono::system_clock::now(), 
             FileState initialState = FileState::Created) 
        : timestamp(time), state(initialState), stateChangeTime(time) {}
};

class FolderWatcherWidget {
private:
    std::string m_name;
    std::string m_folderPath;
    std::mutex m_filesMutex;
    std::unordered_map<std::string, FileInfo> m_fileMap;
    std::vector<std::pair<std::string, FileInfo>> m_recentChanges;

    const float m_minBoxSize = 4.0f;  // Minimum box size
    const float m_maxBoxSize = 80.0f;  // Maximum box size
    const float m_boxSpacing = 2.0f;   // Reduced spacing

    // Colors for different file states
    const ImVec4 m_colorCreated = ImVec4(0.5f, 0.5f, 0.8f, 1.0f);  // Blue for created files
    const ImVec4 m_colorModified = ImVec4(0.8f, 0.6f, 0.2f, 1.0f); // Orange for modified files
    const ImVec4 m_colorAccessed = ImVec4(0.2f, 0.7f, 0.3f, 1.0f); // Green for accessed files

    // Color fade duration in seconds
    const float m_colorFadeDuration = 5.0f;

    // inotify related variables
    int m_inotifyFd = -1;
    int m_watchFd = -1;
    std::atomic<bool> m_running;
    std::thread m_watchThread;

    // Recursive watching
    bool m_recursiveScan = true;
    std::unordered_map<int, std::string> m_watchDescriptors;
    int m_maxRecursionDepth = 18;

public:
    FolderWatcherWidget(const std::string& name, const std::string& folderPath) 
        : m_name(name), m_folderPath(folderPath), m_running(false) {
        // Initialize inotify
        startWatching();
    }

    ~FolderWatcherWidget() {
        stopWatching();
    }

    void startWatching() {
        if (m_running)
            return;

        // Initialize inotify
        m_inotifyFd = inotify_init1(IN_NONBLOCK);
        if (m_inotifyFd == -1) {
            perror("inotify_init1");
            return;
        }

        // Start the watch thread
        m_running = true;
        m_watchThread = std::thread(&FolderWatcherWidget::watchThreadFunc, this);

        // Add initial watches
        addWatchToDirectory(m_folderPath, "");
    }

    void stopWatching() {
        if (!m_running)
            return;

        m_running = false;

        if (m_watchThread.joinable())
            m_watchThread.join();

        // Clean up inotify resources
        for (const auto& [wd, path] : m_watchDescriptors) {
            inotify_rm_watch(m_inotifyFd, wd);
        }

        if (m_inotifyFd != -1) {
            close(m_inotifyFd);
            m_inotifyFd = -1;
        }

        m_watchDescriptors.clear();
    }

    void addWatchToDirectory(const std::string& dirPath, const std::string& relativePath, int depth = 0) {
        // Check if we've reached the maximum recursion depth
        if (depth > m_maxRecursionDepth)
            return;

        // Add a watch for this directory
        // Include IN_ACCESS to detect when files are read
        uint32_t mask = IN_CREATE | IN_DELETE | IN_MOVED_TO | IN_MOVED_FROM | IN_MODIFY | IN_ACCESS | IN_OPEN;
        int wd = inotify_add_watch(m_inotifyFd, dirPath.c_str(), mask);

        if (wd == -1) {
            fprintf(stderr, "Cannot watch '%s': %s\n", dirPath.c_str(), strerror(errno));
            return;
        }

        // Store the watch descriptor with its relative path
        std::string watchPath = relativePath.empty() ? dirPath : relativePath;
        m_watchDescriptors[wd] = watchPath;

        //if (dirPath.find("speech-dispatch") != std::string::npos)
        //    return;

         

         //If recursive scanning is enabled, add watches to all subdirectories
        if (m_recursiveScan) {
            try {
                for (const auto& entry : fs::directory_iterator(dirPath)) {
                    if (fs::is_symlink(entry)) {
                      struct stat buf;
                      int result;

                      result = lstat(entry.path().string().c_str(), &buf);

                      if (result ==0)
                      continue;

                    }
                    if (fs::is_directory(entry)) {
                        std::string subdir = entry.path().string();

                        std::string subRelPath = relativePath.empty() ? 
                            entry.path().filename().string() : 
                            relativePath + "/" + entry.path().filename().string();

                        addWatchToDirectory(subdir, subRelPath, depth + 1);
                    }
                }
            } catch (const fs::filesystem_error& e) {
                fprintf(stderr, "Error scanning directory '%s': %s\n", dirPath.c_str(), e.what());
            }
        }
    }

    void watchThreadFunc() {
        char buffer[EVENT_BUF_LEN];
        fd_set rfds;
        struct timeval tv;

        while (m_running) {
            // Set up the file descriptor set
            FD_ZERO(&rfds);
            FD_SET(m_inotifyFd, &rfds);

            // Set up the timeout (100ms)
            tv.tv_sec = 0;
            tv.tv_usec = 100000;

            // Wait for events
            int ret = select(m_inotifyFd + 1, &rfds, NULL, NULL, &tv);

            if (ret == -1) {
                if (errno != EINTR) {
                    perror("select");
                }
                continue;
            } else if (ret == 0) {
                // Timeout, continue
                continue;
            }

            // Read events
            int length = read(m_inotifyFd, buffer, EVENT_BUF_LEN);

            if (length == -1) {
                if (errno != EAGAIN) {
                    perror("read");
                }
                continue;
            }

            // Process events
            int i = 0;
            while (i < length) {
                struct inotify_event* event = (struct inotify_event*)&buffer[i];

                if (event->len) {
                    // Get the directory path for this watch descriptor
                    auto it = m_watchDescriptors.find(event->wd);
                    if (it != m_watchDescriptors.end()) {
                        std::string dirPath = it->second;
                        std::string filePath = dirPath + "/" + event->name;

                        // Handle the event
                        if (event->mask & IN_CREATE || event->mask & IN_MOVED_TO) {
                            // File or directory created or moved into watched directory
                            std::lock_guard<std::mutex> lock(m_filesMutex);

                            auto now = std::chrono::system_clock::now();
                            m_fileMap[filePath] = FileInfo(now, FileState::Created);

                            // Add to recent changes
                            updateRecentChanges(filePath, m_fileMap[filePath]);

                            // If a directory was created and recursive watching is enabled, add a watch to it
                            if ((event->mask & IN_ISDIR) && m_recursiveScan) {
                                std::string fullPath = m_folderPath;
                                if (!dirPath.empty() && dirPath != m_folderPath) {
                                    //fullPath += "/" + dirPath;
                                }
                                //fullPath += "/" + std::string(event->name);

                                // Calculate depth based on number of slashes in path
                                int depth = 0;
                                for (char c : filePath) {
                                    if (c == '/') depth++;
                                }

                                addWatchToDirectory(fullPath, filePath, depth);
                            }
                        } 
                        else if (event->mask & IN_DELETE || event->mask & IN_MOVED_FROM) {
                            // File or directory deleted or moved out of watched directory
                            std::lock_guard<std::mutex> lock(m_filesMutex);

                            // Remove from file map
                            m_fileMap.erase(filePath);

                            // If it was a directory, remove all watches for its subdirectories
                            if (event->mask & IN_ISDIR) {
                                std::string prefix = filePath + "/";

                                // Remove all files in this directory from the file map
                                for (auto it = m_fileMap.begin(); it != m_fileMap.end();) {
                                    if (it->first.find(prefix) == 0) {
                                        it = m_fileMap.erase(it);
                                    } else {
                                        ++it;
                                    }
                                }

                                // Remove all watches for this directory and its subdirectories
                                for (auto it = m_watchDescriptors.begin(); it != m_watchDescriptors.end();) {
                                    if (it->second.find(prefix) == 0 || it->second == filePath) {
                                        inotify_rm_watch(m_inotifyFd, it->first);
                                        it = m_watchDescriptors.erase(it);
                                    } else {
                                        ++it;
                                    }
                                }
                            }
                        }
                        else if (event->mask & IN_MODIFY) {
                            // File was modified
                            std::lock_guard<std::mutex> lock(m_filesMutex);

                            auto it = m_fileMap.find(filePath);
                            if (it != m_fileMap.end()) {
                                auto now = std::chrono::system_clock::now();
                                it->second.state = FileState::Modified;
                                it->second.stateChangeTime = now;

                                // Add to recent changes
                                updateRecentChanges(filePath, it->second);
                            }
                        }
                        else if ((event->mask & IN_ACCESS) || (event->mask & IN_OPEN)) {
                            // File was accessed or opened
                            std::lock_guard<std::mutex> lock(m_filesMutex);

                            auto it = m_fileMap.find(filePath);
                            if (it != m_fileMap.end()) {
                                auto now = std::chrono::system_clock::now();
                                it->second.state = FileState::Accessed;
                                it->second.stateChangeTime = now;

                                // Add to recent changes
                                updateRecentChanges(filePath, it->second);
                            }
                        }
                    }
                }

                // Move to next event
                i += sizeof(struct inotify_event) + event->len;
            }
        }
    }

    void updateRecentChanges(const std::string& filePath, const FileInfo& info) {
        // Add to recent changes
        auto recentIt = std::find_if(m_recentChanges.begin(), m_recentChanges.end(),
            [&filePath](const auto& pair) { return pair.first == filePath; });

        if (recentIt != m_recentChanges.end()) {
            recentIt->second = info;
        } else {
            m_recentChanges.emplace_back(filePath, info);
        }

        // Sort recent changes (newest first)
        std::sort(m_recentChanges.begin(), m_recentChanges.end(),
            [](const auto& a, const auto& b) { return a.second.stateChangeTime > b.second.stateChangeTime; });

        // Keep only the 20 most recent changes
        if (m_recentChanges.size() > 20) {
            m_recentChanges.resize(20);
        }
    }

    ImVec4 getColorForFile(const FileInfo& info) {
        // Calculate how long since the state changed
        auto now = std::chrono::system_clock::now();
        float secondsSinceChange = std::chrono::duration<float>(now - info.stateChangeTime).count();

        // Get the base color for this state
        ImVec4 baseColor;
        switch (info.state) {
            case FileState::Created:
                baseColor = m_colorCreated;
                break;
            case FileState::Modified:
                baseColor = m_colorModified;
                break;
            case FileState::Accessed:
                baseColor = m_colorAccessed;
                break;
            default:
                baseColor = m_colorCreated;
                break;
        }

        // If the state changed recently, use the full color
        if (secondsSinceChange < m_colorFadeDuration) {
            return baseColor;
        }

        // Otherwise, fade back to the default color (created)
        float t = std::min(1.0f, (secondsSinceChange - m_colorFadeDuration) / m_colorFadeDuration);
        return ImVec4(
            baseColor.x + (m_colorCreated.x - baseColor.x) * t,
            baseColor.y + (m_colorCreated.y - baseColor.y) * t,
            baseColor.z + (m_colorCreated.z - baseColor.z) * t,
            baseColor.w
        );
    }

    std::string getStateString(FileState state) {
        switch (state) {
            case FileState::Created:
                return "Created";
            case FileState::Modified:
                return "Modified";
            case FileState::Accessed:
                return "Accessed";
            case FileState::Deleted:
                return "Deleted";
            default:
                return "Unknown";
        }
    }

    void setRecursiveScan(bool recursive) {
        if (m_recursiveScan == recursive)
            return;

        m_recursiveScan = recursive;

        // Restart watching with new settings
        stopWatching();
        startWatching();
    }

    void setMaxRecursionDepth(int depth) {
        if (m_maxRecursionDepth == depth)
            return;

        m_maxRecursionDepth = depth;

        // Restart watching with new settings
        stopWatching();
        startWatching();
    }

    void setFolderPath(const std::string& path) {
        if (m_folderPath == path)
            return;

        m_folderPath = path;

        // Clear existing files
        {
            std::lock_guard<std::mutex> lock(m_filesMutex);
            m_fileMap.clear();
            m_recentChanges.clear();
        }

        // Restart watching with new path
        stopWatching();
        startWatching();
    }

    void render() {
        ImGui::Begin(("Folder Watcher (inotify) " + m_name).c_str());

        // Folder path input
        char buffer[256];
        strncpy(buffer, m_folderPath.c_str(), sizeof(buffer));
        buffer[sizeof(buffer) - 1] = 0;

        ImGui::Text("Folder to watch:");
        if (ImGui::InputText("##folderPath", buffer, sizeof(buffer), ImGuiInputTextFlags_EnterReturnsTrue)) {
            setFolderPath(buffer);
        }

        ImGui::SameLine();
        if (ImGui::Button("Browse")) {
            // In a real application, you would open a file dialog here
            setFolderPath(".");
        }

        // Recursive scanning option
        bool recursive = m_recursiveScan;
        if (ImGui::Checkbox("Watch Subdirectories", &recursive)) {
            setRecursiveScan(recursive);
        }

        if (recursive) {
            ImGui::SameLine();
            int depth = m_maxRecursionDepth;
            if (ImGui::SliderInt("Max Depth", &depth, 1, 10)) {
                setMaxRecursionDepth(depth);
            }
        }

        // Color legend
        ImGui::Separator();
        ImGui::Text("Color Legend:");

        ImGui::ColorButton("##created", m_colorCreated, 0, ImVec2(20, 20));
        ImGui::SameLine();
        ImGui::Text("Created");

        ImGui::SameLine(150);
        ImGui::ColorButton("##modified", m_colorModified, 0, ImVec2(20, 20));
        ImGui::SameLine();
        ImGui::Text("Modified");

        ImGui::SameLine(300);
        ImGui::ColorButton("##accessed", m_colorAccessed, 0, ImVec2(20, 20));
        ImGui::SameLine();
        ImGui::Text("Accessed/Read");

        ImGui::Separator();

        // Copy the file map to avoid holding the lock during rendering
        std::unordered_map<std::string, FileInfo> fileMap;
        std::vector<std::pair<std::string, FileInfo>> recentChanges;
        {
            std::lock_guard<std::mutex> lock(m_filesMutex);
            fileMap = m_fileMap;
            recentChanges = m_recentChanges;
        }

        ImGui::Text("Files detected: %d", (int)fileMap.size());
        ImGui::Separator();

        // Calculate optimal box size based on number of files and available space
        ImVec2 availableSpace = ImGui::GetContentRegionAvail();
        availableSpace.y = 300.0f; // Fixed height for file boxes area

        // Calculate optimal grid dimensions
        int fileCount = (int)fileMap.size();
        if (fileCount == 0) fileCount = 1; // Avoid division by zero

        // Calculate the ideal number of rows and columns
        float aspectRatio = availableSpace.x / availableSpace.y;
        float idealColumns = std::sqrt(fileCount * aspectRatio);
        float idealRows = fileCount / idealColumns;

        // Round to get actual grid dimensions
        int columns = std::max(1, (int)std::ceil(idealColumns));
        int rows = std::max(1, (int)std::ceil((float)fileCount / columns));

        // Calculate box size based on available space and grid dimensions
        float boxWidth = (availableSpace.x - (columns - 1) * m_boxSpacing) / columns;
        float boxHeight = (availableSpace.y - (rows - 1) * m_boxSpacing - rows * 20.0f) / rows; // Account for text below boxes

        // Determine the final box size (square)
        float boxSize = std::min(boxWidth, boxHeight);

        // Clamp box size between min and max
        boxSize = std::max(m_minBoxSize, std::min(m_maxBoxSize, boxSize));

        ImGui::Text("File Visualization:");
        ImGui::BeginChild("FileBoxes", ImVec2(0, 300), true);

        // Calculate actual columns based on final box size
        int actualColumns = std::max(1, (int)((availableSpace.x + m_boxSpacing) / (boxSize + m_boxSpacing)));

        int columnIndex = 0;
        for (const auto& [filepath, fileInfo] : fileMap) {
            if (columnIndex > 0) {
                ImGui::SameLine();
            }

            // Extract filename from path for display
            std::string displayPath = filepath;
            size_t lastSlash = displayPath.find_last_of("/\\");
            std::string filename = (lastSlash != std::string::npos) ? 
                displayPath.substr(lastSlash + 1) : displayPath;

            // Create a box for the file
            ImGui::BeginGroup();
            ImGui::PushID(filepath.c_str());

            // Draw the box
            ImVec2 cursorPos = ImGui::GetCursorScreenPos();
            ImDrawList* drawList = ImGui::GetWindowDrawList();

            // Get color based on file state
            ImVec4 boxColor = getColorForFile(fileInfo);

            // Box background
            drawList->AddRectFilled(
                cursorPos,
                ImVec2(cursorPos.x + boxSize, cursorPos.y + boxSize),
                ImGui::ColorConvertFloat4ToU32(boxColor)
            );

            // Box border
            drawList->AddRect(
                cursorPos,
                ImVec2(cursorPos.x + boxSize, cursorPos.y + boxSize),
                IM_COL32(255, 255, 255, 128)
            );

            // Make the box clickable
            ImGui::InvisibleButton("##box", ImVec2(boxSize, boxSize));

            // Show tooltip on hover with full path and state
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::Text("Path: %s", filepath.c_str());
                ImGui::Text("State: %s", getStateString(fileInfo.state).c_str());

                // Convert time_point to readable time
                auto time_t_value = std::chrono::system_clock::to_time_t(fileInfo.stateChangeTime);
                std::tm* tm = std::localtime(&time_t_value);

                char timeBuffer[64];
                std::strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", tm);
                ImGui::Text("Last change: %s", timeBuffer);

                ImGui::EndTooltip();
            }

            // File name (truncated if needed)
            int maxChars = std::max(3, (int)(boxSize / 8)); // Rough estimate of chars that fit

            if (filename.length() > maxChars) {
                filename = filename.substr(0, maxChars - 3) + "...";
            }

            // Center the text in the box
            float textWidth = ImGui::CalcTextSize(filename.c_str()).x;
            ImGui::SetCursorPosX(cursorPos.x - ImGui::GetCursorScreenPos().x + (boxSize - textWidth) * 0.5f);
            ImGui::SetCursorPosY(cursorPos.y - ImGui::GetCursorScreenPos().y + boxSize + 2.0f);
            ImGui::Text("%s", filename.c_str());

            ImGui::PopID();
            ImGui::EndGroup();

            // Move to next column or row
            columnIndex++;
            if (columnIndex >= actualColumns) {
                columnIndex = 0;
                ImGui::Dummy(ImVec2(0, boxSize + 20.0f)); // Space for the box and text below
            }
        }

        ImGui::EndChild();

        // Recent changes section
        ImGui::Separator();
        ImGui::Text("Recent Changes:");
        ImGui::BeginChild("RecentChanges", ImVec2(0, 150), true);

        for (const auto& [filepath, fileInfo] : recentChanges) {
            // Convert time_point to readable time
            auto time_t_value = std::chrono::system_clock::to_time_t(fileInfo.stateChangeTime);
            std::tm* tm = std::localtime(&time_t_value);

            char timeBuffer[64];
            std::strftime(timeBuffer, sizeof(timeBuffer), "%H:%M:%S", tm);

            // Get color for the state
            ImVec4 textColor;
            switch (fileInfo.state) {
                case FileState::Created:
                    textColor = m_colorCreated;
                    break;
                case FileState::Modified:
                    textColor = m_colorModified;
                    break;
                case FileState::Accessed:
                    textColor = m_colorAccessed;
                    break;
                default:
                    textColor = ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
                    break;
            }

            ImGui::TextColored(textColor, "[%s] %s (%s)", 
                timeBuffer, filepath.c_str(), getStateString(fileInfo.state).c_str());
        }

        ImGui::EndChild();
        ImGui::End();
    }
};

