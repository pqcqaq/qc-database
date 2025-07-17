#include "cache/cache_server.h"
#include "cache/common/types.h"
#include <iostream>
#include <string>
#include <sstream>
#include <signal.h>
#include <unistd.h>
#include <getopt.h>

using namespace cache;

// Global server instance for signal handling
std::unique_ptr<CacheServer> g_server;

void print_usage(const char* program_name) {
    std::cout << "Usage: " << program_name << " [OPTIONS]\n\n"
              << "Options:\n"
              << "  -c, --config FILE     Configuration file path (default: config.yaml)\n"
              << "  -p, --port PORT       Listen port (default: 8080)\n"
              << "  -a, --address ADDR    Listen address (default: 0.0.0.0)\n"
              << "  -d, --data-dir DIR    Data directory (default: ./data)\n"
              << "  -n, --node-id ID      Node ID for cluster mode\n"
              << "  -s, --seeds NODES     Comma-separated list of seed nodes\n"
              << "  -l, --log-level LEVEL Log level (debug, info, warn, error)\n"
              << "  -D, --daemon          Run as daemon\n"
              << "  -h, --help           Show this help message\n"
              << "  -v, --version        Show version information\n\n"
              << "Examples:\n"
              << "  " << program_name << " --config /etc/cache/config.yaml\n"
              << "  " << program_name << " --port 9000 --data-dir /var/lib/cache\n"
              << "  " << program_name << " --node-id node1 --seeds node2:8080,node3:8080\n\n";
}

void print_version() {
    std::cout << "Distributed Cache Server v1.0.0\n"
              << "Built with C++17, supports multi-level key-value storage\n"
              << "Features: LSM-Tree storage, Raft consensus, consistent hashing\n\n";
}

void signal_handler(int signal) {
    std::cout << "\nReceived signal " << signal << ", shutting down gracefully...\n";
    if (g_server) {
        auto result = g_server->stop();
        if (!result.is_ok()) {
            std::cerr << "Error during shutdown: " << result.error_message << "\n";
        }
    }
    exit(0);
}

void setup_signal_handlers() {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);
    signal(SIGQUIT, signal_handler);
    
    // Ignore SIGPIPE
    signal(SIGPIPE, SIG_IGN);
}

bool daemonize() {
    pid_t pid = fork();
    
    if (pid < 0) {
        std::cerr << "Failed to fork process\n";
        return false;
    }
    
    if (pid > 0) {
        // Parent process exits
        exit(0);
    }
    
    // Create new session
    if (setsid() < 0) {
        std::cerr << "Failed to create new session\n";
        return false;
    }
    
    // Fork again to ensure we're not session leader
    pid = fork();
    if (pid < 0) {
        std::cerr << "Failed to fork process (second time)\n";
        return false;
    }
    
    if (pid > 0) {
        exit(0);
    }
    
    // Change working directory to root
    if (chdir("/") < 0) {
        std::cerr << "Failed to change working directory\n";
        return false;
    }
    
    // Close standard file descriptors
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
    
    return true;
}

int main(int argc, char* argv[]) {
    // Command line options
    std::string config_file = "config.yaml";
    std::string listen_address = "0.0.0.0";
    uint16_t listen_port = 8080;
    std::string data_dir = "./data";
    std::string node_id;
    std::string seed_nodes;
    std::string log_level = "info";
    bool run_as_daemon = false;
    
    // Parse command line arguments
    static struct option long_options[] = {
        {"config", required_argument, 0, 'c'},
        {"port", required_argument, 0, 'p'},
        {"address", required_argument, 0, 'a'},
        {"data-dir", required_argument, 0, 'd'},
        {"node-id", required_argument, 0, 'n'},
        {"seeds", required_argument, 0, 's'},
        {"log-level", required_argument, 0, 'l'},
        {"daemon", no_argument, 0, 'D'},
        {"help", no_argument, 0, 'h'},
        {"version", no_argument, 0, 'v'},
        {0, 0, 0, 0}
    };
    
    int c;
    int option_index = 0;
    
    while ((c = getopt_long(argc, argv, "c:p:a:d:n:s:l:Dhv", long_options, &option_index)) != -1) {
        switch (c) {
            case 'c':
                config_file = optarg;
                break;
            case 'p':
                listen_port = static_cast<uint16_t>(std::stoi(optarg));
                break;
            case 'a':
                listen_address = optarg;
                break;
            case 'd':
                data_dir = optarg;
                break;
            case 'n':
                node_id = optarg;
                break;
            case 's':
                seed_nodes = optarg;
                break;
            case 'l':
                log_level = optarg;
                break;
            case 'D':
                run_as_daemon = true;
                break;
            case 'h':
                print_usage(argv[0]);
                return 0;
            case 'v':
                print_version();
                return 0;
            case '?':
                std::cerr << "Unknown option. Use --help for usage information.\n";
                return 1;
            default:
                break;
        }
    }
    
    try {
        // Setup signal handlers
        setup_signal_handlers();
        
        // Run as daemon if requested
        if (run_as_daemon) {
            if (!daemonize()) {
                std::cerr << "Failed to daemonize process\n";
                return 1;
            }
        }
        
        // Load or create configuration
        ServerConfig config;
        
        // Try to load from config file
        if (!config_file.empty()) {
            auto config_result = ServerConfig::load_from_file(config_file);
            if (config_result.is_ok()) {
                config = config_result.data;
                std::cout << "Loaded configuration from " << config_file << "\n";
            } else {
                std::cout << "Could not load config file, using default configuration\n";
            }
        }
        
        // Override with command line arguments
        config.listen_address = listen_address;
        config.listen_port = listen_port;
        config.data_directory = data_dir;
        
        if (!node_id.empty()) {
            config.cluster_config.node_id = node_id;
        }
        
        if (!seed_nodes.empty()) {
            config.cluster_config.seed_nodes.clear();
            std::stringstream ss(seed_nodes);
            std::string node;
            while (std::getline(ss, node, ',')) {
                config.cluster_config.seed_nodes.push_back(node);
            }
        }
        
        // Apply environment variable overrides
        config::apply_env_overrides(config);
        
        // Validate configuration
        auto validation_result = config.validate();
        if (!validation_result.is_ok()) {
            std::cerr << "Configuration validation failed: " << validation_result.error_message << "\n";
            return 1;
        }
        
        // Create and start server
        std::cout << "Starting Distributed Cache Server...\n";
        std::cout << "Listen address: " << config.listen_address << ":" << config.listen_port << "\n";
        std::cout << "Data directory: " << config.data_directory << "\n";
        std::cout << "Node ID: " << config.cluster_config.node_id << "\n";
        
        g_server = std::make_unique<CacheServer>(config);
        
        auto start_result = g_server->start();
        if (!start_result.is_ok()) {
            std::cerr << "Failed to start server: " << start_result.error_message << "\n";
            return 1;
        }
        
        std::cout << "Server started successfully!\n";
        
        // Print server information
        auto stats = g_server->get_stats();
        std::cout << "Server statistics:\n";
        std::cout << "  - Cluster size: " << stats.cluster_size << "\n";
        std::cout << "  - Is leader: " << (stats.is_leader ? "yes" : "no") << "\n";
        std::cout << "  - Storage engine: LSM-Tree\n";
        std::cout << "  - Authentication: " << (config.enable_authentication ? "enabled" : "disabled") << "\n";
        std::cout << "  - Monitoring: " << (config.enable_monitoring ? "enabled" : "disabled") << "\n";
        
        if (config.enable_monitoring) {
            std::cout << "  - Metrics endpoint: http://" << config.listen_address 
                      << ":" << config.metrics_port << "/metrics\n";
        }
        
        std::cout << "\nServer is ready to accept connections. Press Ctrl+C to stop.\n";
        
        // Keep the main thread alive
        while (g_server->is_running()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        
    } catch (const std::exception& e) {
        std::cerr << "Fatal error: " << e.what() << "\n";
        return 1;
    }
    
    std::cout << "Server shutdown complete.\n";
    return 0;
}