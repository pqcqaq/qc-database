# Makefile for Distributed Cache Server
# This is a convenience wrapper around CMake

# Default configuration
BUILD_TYPE ?= Release
BUILD_DIR ?= build
JOBS ?= $(shell nproc)

# Colors for output
RED = \033[0;31m
GREEN = \033[0;32m
YELLOW = \033[1;33m
BLUE = \033[0;34m
NC = \033[0m

# Default target
.PHONY: all
all: build

# Build the project
.PHONY: build
build:
	@echo -e "$(BLUE)[INFO]$(NC) Building project..."
	@mkdir -p $(BUILD_DIR)
	@cd $(BUILD_DIR) && cmake -DCMAKE_BUILD_TYPE=$(BUILD_TYPE) ..
	@cd $(BUILD_DIR) && make -j$(JOBS)
	@echo -e "$(GREEN)[SUCCESS]$(NC) Build completed!"

# Clean build
.PHONY: clean
clean:
	@echo -e "$(BLUE)[INFO]$(NC) Cleaning build directory..."
	@rm -rf $(BUILD_DIR)
	@echo -e "$(GREEN)[SUCCESS]$(NC) Clean completed!"

# Rebuild (clean + build)
.PHONY: rebuild
rebuild: clean build

# Debug build
.PHONY: debug
debug:
	@$(MAKE) build BUILD_TYPE=Debug

# Release build
.PHONY: release
release:
	@$(MAKE) build BUILD_TYPE=Release

# Run tests
.PHONY: test
test: build
	@echo -e "$(BLUE)[INFO]$(NC) Running tests..."
	@cd $(BUILD_DIR) && ctest --output-on-failure
	@echo -e "$(GREEN)[SUCCESS]$(NC) Tests completed!"

# Install
.PHONY: install
install: build
	@echo -e "$(BLUE)[INFO]$(NC) Installing..."
	@cd $(BUILD_DIR) && sudo make install
	@echo -e "$(GREEN)[SUCCESS]$(NC) Installation completed!"

# Format code
.PHONY: format
format:
	@echo -e "$(BLUE)[INFO]$(NC) Formatting code..."
	@find include src tests examples -name "*.cpp" -o -name "*.h" | xargs clang-format -i
	@echo -e "$(GREEN)[SUCCESS]$(NC) Code formatting completed!"

# Run static analysis
.PHONY: lint
lint:
	@echo -e "$(BLUE)[INFO]$(NC) Running static analysis..."
	@find src include -name "*.cpp" -o -name "*.h" | xargs cppcheck --enable=all --suppress=missingIncludeSystem
	@echo -e "$(GREEN)[SUCCESS]$(NC) Static analysis completed!"

# Generate documentation
.PHONY: docs
docs:
	@echo -e "$(BLUE)[INFO]$(NC) Generating documentation..."
	@doxygen Doxyfile
	@echo -e "$(GREEN)[SUCCESS]$(NC) Documentation generated in docs/html/"

# Docker build
.PHONY: docker-build
docker-build:
	@echo -e "$(BLUE)[INFO]$(NC) Building Docker image..."
	@docker build -t distributed-cache .
	@echo -e "$(GREEN)[SUCCESS]$(NC) Docker image built!"

# Docker run single node
.PHONY: docker-run
docker-run: docker-build
	@echo -e "$(BLUE)[INFO]$(NC) Starting Docker container..."
	@docker run -d --name cache-server -p 8080:8080 -p 9090:9090 distributed-cache
	@echo -e "$(GREEN)[SUCCESS]$(NC) Container started!"

# Docker cluster
.PHONY: docker-cluster
docker-cluster:
	@echo -e "$(BLUE)[INFO]$(NC) Starting Docker cluster..."
	@docker-compose up -d
	@echo -e "$(GREEN)[SUCCESS]$(NC) Cluster started!"

# Stop Docker cluster
.PHONY: docker-stop
docker-stop:
	@echo -e "$(BLUE)[INFO]$(NC) Stopping Docker cluster..."
	@docker-compose down
	@echo -e "$(GREEN)[SUCCESS]$(NC) Cluster stopped!"

# Run server locally
.PHONY: run
run: build
	@echo -e "$(BLUE)[INFO]$(NC) Starting cache server..."
	@$(BUILD_DIR)/cache_server --config config/cache_config.yaml

# Run with custom config
.PHONY: run-config
run-config: build
	@echo -e "$(BLUE)[INFO]$(NC) Starting cache server with custom config..."
	@$(BUILD_DIR)/cache_server --config $(CONFIG)

# Benchmark
.PHONY: benchmark
benchmark: build
	@echo -e "$(BLUE)[INFO]$(NC) Running benchmark..."
	@$(BUILD_DIR)/examples/benchmark --duration 60 --threads 4
	@echo -e "$(GREEN)[SUCCESS]$(NC) Benchmark completed!"

# Memory check with Valgrind
.PHONY: memcheck
memcheck: debug
	@echo -e "$(BLUE)[INFO]$(NC) Running memory check..."
	@valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all $(BUILD_DIR)/cache_server --help
	@echo -e "$(GREEN)[SUCCESS]$(NC) Memory check completed!"

# Performance profiling
.PHONY: profile
profile: debug
	@echo -e "$(BLUE)[INFO]$(NC) Running performance profiling..."
	@perf record -g $(BUILD_DIR)/cache_server --help
	@perf report
	@echo -e "$(GREEN)[SUCCESS]$(NC) Profiling completed!"

# Package for distribution
.PHONY: package
package: build
	@echo -e "$(BLUE)[INFO]$(NC) Creating package..."
	@cd $(BUILD_DIR) && cpack
	@echo -e "$(GREEN)[SUCCESS]$(NC) Package created!"

# Check dependencies
.PHONY: check-deps
check-deps:
	@echo -e "$(BLUE)[INFO]$(NC) Checking dependencies..."
	@./scripts/check_dependencies.sh
	@echo -e "$(GREEN)[SUCCESS]$(NC) Dependencies check completed!"

# Show help
.PHONY: help
help:
	@echo "Distributed Cache Server - Build System"
	@echo ""
	@echo "Available targets:"
	@echo "  build        - Build the project (default)"
	@echo "  clean        - Clean build directory"
	@echo "  rebuild      - Clean and build"
	@echo "  debug        - Build in debug mode"
	@echo "  release      - Build in release mode"
	@echo "  test         - Run unit tests"
	@echo "  install      - Install the project"
	@echo "  format       - Format source code"
	@echo "  lint         - Run static analysis"
	@echo "  docs         - Generate documentation"
	@echo "  docker-build - Build Docker image"
	@echo "  docker-run   - Run single node in Docker"
	@echo "  docker-cluster - Start Docker cluster"
	@echo "  docker-stop  - Stop Docker cluster"
	@echo "  run          - Run server locally"
	@echo "  run-config   - Run with custom config (CONFIG=path)"
	@echo "  benchmark    - Run performance benchmark"
	@echo "  memcheck     - Run memory check with Valgrind"
	@echo "  profile      - Run performance profiling"
	@echo "  package      - Create distribution package"
	@echo "  check-deps   - Check build dependencies"
	@echo "  help         - Show this help message"
	@echo ""
	@echo "Variables:"
	@echo "  BUILD_TYPE   - Build type (Release, Debug) [default: Release]"
	@echo "  BUILD_DIR    - Build directory [default: build]"
	@echo "  JOBS         - Number of parallel jobs [default: $(JOBS)]"
	@echo "  CONFIG       - Configuration file for run-config"
	@echo ""
	@echo "Examples:"
	@echo "  make build BUILD_TYPE=Debug"
	@echo "  make test JOBS=2"
	@echo "  make run-config CONFIG=config/production.yaml"

# Show project status
.PHONY: status
status:
	@echo "Project Status:"
	@echo "  Build directory: $(BUILD_DIR)"
	@echo "  Build type: $(BUILD_TYPE)"
	@echo "  Parallel jobs: $(JOBS)"
	@if [ -f "$(BUILD_DIR)/cache_server" ]; then \
		echo "  Binary: $(BUILD_DIR)/cache_server (exists)"; \
		echo "  Binary size: $$(du -h $(BUILD_DIR)/cache_server | cut -f1)"; \
	else \
		echo "  Binary: $(BUILD_DIR)/cache_server (not built)"; \
	fi
	@if [ -d "$(BUILD_DIR)" ]; then \
		echo "  Build directory size: $$(du -sh $(BUILD_DIR) | cut -f1)"; \
	fi