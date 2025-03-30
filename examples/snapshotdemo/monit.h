class LinuxNetworkMonitor {
private:
    std::string m_interface;
    uint64_t m_lastBytesReceived;
    uint64_t m_cumulativeBytesReceived;
    std::chrono::time_point<std::chrono::steady_clock> m_lastCheckTime;
    std::chrono::time_point<std::chrono::steady_clock> m_startTime;
    float m_currentRate;
    bool m_firstRun;

public:
    LinuxNetworkMonitor(const std::string& interface = "") 
        : m_interface(interface), 
          m_lastBytesReceived(0), 
          m_cumulativeBytesReceived(0),
          m_currentRate(0.0f),
          m_firstRun(true) {

        // If no interface specified, try to find the default one
        if (m_interface.empty()) {
            m_interface = GetDefaultInterface();
        }

        // Initialize with current values
        m_lastBytesReceived = GetTotalBytesReceived(m_interface);
        m_lastCheckTime = std::chrono::steady_clock::now();
        m_startTime = m_lastCheckTime;
    }

    float GetCurrentNetworkUsage() {
        auto currentTime = std::chrono::steady_clock::now();
        float deltaSeconds = std::chrono::duration<float>(currentTime - m_lastCheckTime).count();

        // Don't update too frequently to avoid division by very small numbers
        if (deltaSeconds < 0.05f) {
            return m_currentRate;
        }

        uint64_t currentBytes = GetTotalBytesReceived(m_interface);
        uint64_t bytesReceived = 0;

        if (m_firstRun) {
            // First run, don't count existing bytes as downloaded
            m_firstRun = false;
            m_lastBytesReceived = currentBytes;
            return 0.0f;
        }

        // Handle counter reset or first call
        if (currentBytes >= m_lastBytesReceived) {
            bytesReceived = currentBytes - m_lastBytesReceived;
        } else {
            // Counter might have reset or overflowed
            bytesReceived = currentBytes;
        }

        // Add to cumulative total
        m_cumulativeBytesReceived += bytesReceived;

        // Calculate bytes per second
        m_currentRate = static_cast<float>(bytesReceived) / deltaSeconds;

        // Update last values
        m_lastBytesReceived = currentBytes;
        m_lastCheckTime = currentTime;

        return m_currentRate;
    }

    // Get total bytes downloaded since monitoring began
    uint64_t GetCumulativeBytesReceived() const {
        return m_cumulativeBytesReceived;
    }

    // Get average download rate since monitoring began
    float GetAverageDownloadRate() const {
        auto currentTime = std::chrono::steady_clock::now();
        float totalSeconds = std::chrono::duration<float>(currentTime - m_startTime).count();
        if (totalSeconds > 0.0f) {
            return static_cast<float>(m_cumulativeBytesReceived) / totalSeconds;
        }
        return 0.0f;
    }

    // Reset cumulative statistics
    void ResetStatistics() {
        m_cumulativeBytesReceived = 0;
        m_startTime = std::chrono::steady_clock::now();
    }

    std::string GetDefaultInterface() {
        // Try to find the default interface by checking /proc/net/route
        std::ifstream routeFile("/proc/net/route");
        std::string line;

        if (routeFile.is_open()) {
            // Skip header line
            std::getline(routeFile, line);

            while (std::getline(routeFile, line)) {
                std::istringstream iss(line);
                std::string iface, dest, gateway;
                iss >> iface >> dest;

                // Default route has destination 00000000
                if (dest == "00000000") {
                    return iface;
                }
            }
        }

        // Fallback to common interfaces if default route not found
        const char* commonInterfaces[] = {"eth0", "wlan0", "enp0s3", "ens33", "wlp2s0"};
        for (const auto& iface : commonInterfaces) {
            if (InterfaceExists(iface)) {
                return iface;
            }
        }

        // Last resort, return first available interface
        std::ifstream netdevFile("/proc/net/dev");
        if (netdevFile.is_open()) {
            // Skip header lines
            std::getline(netdevFile, line);
            std::getline(netdevFile, line);

            while (std::getline(netdevFile, line)) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string iface = line.substr(0, pos);
                    // Trim whitespace
                    iface.erase(0, iface.find_first_not_of(" \t"));
                    return iface;
                }
            }
        }

        return "lo"; // Loopback as absolute fallback
    }

    bool InterfaceExists(const std::string& interface) {
        std::ifstream netdevFile("/proc/net/dev");
        std::string line;

        if (netdevFile.is_open()) {
            while (std::getline(netdevFile, line)) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string iface = line.substr(0, pos);
                    // Trim whitespace
                    iface.erase(0, iface.find_first_not_of(" \t"));
                    if (iface == interface) {
                        return true;
                    }
                }
            }
        }

        return false;
    }

    uint64_t GetTotalBytesReceived(const std::string& interface) {
        std::ifstream netdevFile("/proc/net/dev");
        std::string line;

        if (netdevFile.is_open()) {
            while (std::getline(netdevFile, line)) {
                // Find the line for our interface
                size_t pos = line.find(interface + ":");
                if (pos != std::string::npos) {
                    std::istringstream iss(line.substr(pos + interface.length() + 1));
                    uint64_t bytesReceived;
                    iss >> bytesReceived; // First value after interface name is bytes received
                    return bytesReceived;
                }
            }
        }

        return 0; // Return 0 if interface not found or file couldn't be opened
    }

    const std::string& GetInterface() const {
        return m_interface;
    }

    void SetInterface(const std::string& interface) {
        if (m_interface != interface) {
            m_interface = interface;
            m_lastBytesReceived = GetTotalBytesReceived(m_interface);
            m_lastCheckTime = std::chrono::steady_clock::now();
            m_currentRate = 0.0f;
            m_firstRun = true;
            ResetStatistics();
        }
    }

    std::vector<std::string> GetAvailableInterfaces() {
        std::vector<std::string> interfaces;
        std::ifstream netdevFile("/proc/net/dev");
        std::string line;

        if (netdevFile.is_open()) {
            // Skip header lines
            std::getline(netdevFile, line);
            std::getline(netdevFile, line);

            while (std::getline(netdevFile, line)) {
                size_t pos = line.find(':');
                if (pos != std::string::npos) {
                    std::string iface = line.substr(0, pos);
                    // Trim whitespace
                    iface.erase(0, iface.find_first_not_of(" \t"));
                    interfaces.push_back(iface);
                }
            }
        }

        return interfaces;
    }
};

