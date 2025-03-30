#include <iostream>
#include <vector>
#include <string>
#include <chrono>
#include <thread>
#include <fstream>
#include <sstream>
#include <cstring>

// ImGui and GLFW includes
#include "imgui.h"
#include "imgui_impl_glfw.h"
#include "imgui_impl_opengl3.h"
#include <GLFW/glfw3.h>
#include "monit.h"


// Network Usage Widget Implementation
class NetworkUsageWidget {
private:
    LinuxNetworkMonitor m_networkMonitor;
    std::vector<float> m_downloadHistory;
    int m_maxPoints;
    float m_maxValue;
    std::chrono::time_point<std::chrono::steady_clock> m_lastUpdateTime;
    float m_currentDownloadRate;
    double m_totalDownloadRate;
    std::chrono::time_point<std::chrono::steady_clock> m_sessionStartTime;

    std::string FormatBytes(float bytes) {
        const char* units[] = {"B", "KB", "MB", "GB", "TB"};
        int unitIndex = 0;
        float value = bytes;

        while (value > 1024.0f && unitIndex < 4) {
            value /= 1024.0f;
            unitIndex++;
        }

        char buffer[32];
        if (unitIndex == 0) {
            snprintf(buffer, sizeof(buffer), "%.0f %s", value, units[unitIndex]);
        } else {
            snprintf(buffer, sizeof(buffer), "%.2f %s", value, units[unitIndex]);
        }
        return std::string(buffer);
    }

    std::string FormatTime(float seconds) {
        int hours = static_cast<int>(seconds) / 3600;
        int minutes = (static_cast<int>(seconds) % 3600) / 60;
        int secs = static_cast<int>(seconds) % 60;

        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%02d:%02d:%02d", hours, minutes, secs);
        return std::string(buffer);
    }

public:
    NetworkUsageWidget(int historySize = 100, const std::string& interface = "") 
        : m_networkMonitor(interface), m_maxPoints(historySize), m_maxValue(1.0f), m_currentDownloadRate(0.0f), m_totalDownloadRate(0.0){
        m_downloadHistory.resize(m_maxPoints, 0.0f);
        m_lastUpdateTime = std::chrono::steady_clock::now();
        m_sessionStartTime = m_lastUpdateTime;
    }

    void Update() {
        auto currentTime = std::chrono::steady_clock::now();
        float deltaTime = std::chrono::duration<float>(currentTime - m_lastUpdateTime).count();

        // Update at most once per 100ms
        if (deltaTime >= 0.1f) {
            m_lastUpdateTime = currentTime;
            m_currentDownloadRate = m_networkMonitor.GetCurrentNetworkUsage();
            m_totalDownloadRate = m_networkMonitor.GetCumulativeBytesReceived();

            // Shift values to the left and add new value
            for (int i = 0; i < m_maxPoints - 1; i++) {
                m_downloadHistory[i] = m_downloadHistory[i + 1];
            }
            m_downloadHistory[m_maxPoints - 1] = m_totalDownloadRate;

            // Update max value for scaling
            m_maxValue = 1.0f;
            for (float value : m_downloadHistory) {
                m_maxValue = std::max(m_maxValue, value);
            }
            m_maxValue = std::max(m_maxValue, 200000000.0f);
            // Add 20% headroom to max value
            m_maxValue *= 1.2f;
        }
    }

    void Render(const char* label, const ImVec2& size = ImVec2(0, 0)) {
        Update();

        ImGui::Begin(label, nullptr, ImGuiWindowFlags_AlwaysAutoResize);

        // Interface selection dropdown
        std::vector<std::string> interfaces = m_networkMonitor.GetAvailableInterfaces();
        std::string currentInterface = m_networkMonitor.GetInterface();

        if (ImGui::BeginCombo("Interface", currentInterface.c_str())) {
            for (const auto& iface : interfaces) {
                bool isSelected = (currentInterface == iface);
                if (ImGui::Selectable(iface.c_str(), isSelected)) {
                    m_networkMonitor.SetInterface(iface);
                    // Reset history when changing interface
                    std::fill(m_downloadHistory.begin(), m_downloadHistory.end(), 0.0f);
                    m_maxValue = 1.0f;
                    m_sessionStartTime = std::chrono::steady_clock::now();
                }
                if (isSelected) {
                    ImGui::SetItemDefaultFocus();
                }
            }
            ImGui::EndCombo();
        }

        ImVec2 graphSize = size;
        if (graphSize.x <= 0) graphSize.x = 300;
        if (graphSize.y <= 0) graphSize.y = 150;

        // Calculate session time
        auto currentTime = std::chrono::steady_clock::now();
        float sessionTime = std::chrono::duration<float>(currentTime - m_sessionStartTime).count();

        // Display current download rate
        ImGui::Text("Current: %s/s", FormatBytes(m_currentDownloadRate).c_str());
        ImGui::Text("Peak: %s/s", FormatBytes(m_maxValue).c_str());
        ImGui::Separator();

        // Display cumulative statistics
        uint64_t totalBytes = m_networkMonitor.GetCumulativeBytesReceived();
        float avgRate = m_networkMonitor.GetAverageDownloadRate();

        //ImGui::Text("Total Downloaded: %s", FormatBytes(static_cast<float>(totalBytes)).c_str());
        ImGui::Text("Average Rate: %s/s", FormatBytes(avgRate).c_str());
        ImGui::Text("Session Time: %s", FormatTime(sessionTime).c_str());

        if (ImGui::Button("Reset Statistics")) {
            m_networkMonitor.ResetStatistics();
            m_sessionStartTime = std::chrono::steady_clock::now();
        }

        ImGui::Separator();

        // Plot the download history
        ImGui::PlotLines("##download", 
                         m_downloadHistory.data(), 
                         m_downloadHistory.size(), 
                         0, 
                         ("Total Downloaded: " + FormatBytes(static_cast<float>(totalBytes))).c_str(), 
                         0.0f, 
                         m_maxValue, 
                         graphSize);

        // Add a color-filled area under the line
        ImDrawList* drawList = ImGui::GetWindowDrawList();
        ImVec2 p0 = ImGui::GetItemRectMin();
        ImVec2 p1 = ImGui::GetItemRectMax();
        ImVec2 sizel = ImVec2(p1.x - p0.x, p1.y - p0.y);

        // Draw gradient-filled area under the curve
        const float width = sizel.x / (float)(m_maxPoints - 1);
        for (int i = 0; i < m_maxPoints - 1; i++) {
            float h1 = m_downloadHistory[i] / m_maxValue;
            float h2 = m_downloadHistory[i + 1] / m_maxValue;

            ImVec2 pos0(p0.x + i * width, p0.y + sizel.y * (1.0f - h1));
            ImVec2 pos1(p0.x + (i + 1) * width, p0.y + sizel.y * (1.0f - h2));
            ImVec2 pos2(p0.x + (i + 1) * width, p1.y);
            ImVec2 pos3(p0.x + i * width, p1.y);

            ImU32 col = ImGui::GetColorU32(ImVec4(0.0f, 0.7f, 1.0f, 0.3f));
            drawList->AddQuadFilled(pos0, pos1, pos2, pos3, col);
            drawList->AddText(ImVec2((p1.x - p0.x) / 2., (p1.y - p0.y) /2.), ImColor(255.0f,255.0f,255.0f,0.0f),FormatBytes(static_cast<float>(totalBytes)).c_str());
        }

        ImGui::End();
    }
};

// Error callback for GLFW
static void glfw_error_callback(int error, const char* description) {
    fprintf(stderr, "GLFW Error %d: %s\n", error, description);
}

int main(int argc, char** argv) {
    // Setup GLFW window
    glfwSetErrorCallback(glfw_error_callback);
    if (!glfwInit()) {
        return 1;
    }

    // GL 3.0 + GLSL 130
    const char* glsl_version = "#version 130";
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

    // Create window with graphics context
    GLFWwindow* window = glfwCreateWindow(800, 600, "Network Monitor", nullptr, nullptr);
    if (window == nullptr) {
        return 1;
    }
    glfwMakeContextCurrent(window);
    glfwSwapInterval(1); // Enable vsync

    // Setup Dear ImGui context
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO(); (void)io;
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;     // Enable Keyboard Controls

    // Setup Dear ImGui style
    ImGui::StyleColorsDark();

    // Setup Platform/Renderer backends
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init(glsl_version);

    // Create our network widget
    NetworkUsageWidget networkWidget(150); // 150 data points history

    // Main loop
    while (!glfwWindowShouldClose(window)) {
        // Poll and handle events
        glfwPollEvents();

        // Start the Dear ImGui frame
        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGui::SetNextWindowPos(ImVec2{});
        ImGui::SetNextWindowSize(ImVec2{800, 600});
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));

        ImGuiWindowFlags window_flags =  ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoResize |
                                        ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoBringToFrontOnFocus |
                                        ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBackground;

        ImGui::Begin("DockSpace", nullptr, window_flags);
        ImGui::PopStyleVar(3);

        ImGui::End();

        // Render our network widget
        networkWidget.Render("Network Download Monitor", ImVec2(600, 300));

        // Rendering
        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    // Cleanup
    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();

    glfwDestroyWindow(window);
    glfwTerminate();

    return 0;
}

