#!/bin/bash

# Build script for Distributed Cache Server
# Usage: ./scripts/build.sh [options]

set -e

# Default values
BUILD_TYPE="Release"
BUILD_DIR="build"
CLEAN_BUILD=false
RUN_TESTS=false
INSTALL=false
VERBOSE=false
JOBS=$(nproc)

# Colors for output
RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
BLUE='\033[0;34m'
NC='\033[0m' # No Color

# Print colored output
print_info() {
    echo -e "${BLUE}[INFO]${NC} $1"
}

print_success() {
    echo -e "${GREEN}[SUCCESS]${NC} $1"
}

print_warning() {
    echo -e "${YELLOW}[WARNING]${NC} $1"
}

print_error() {
    echo -e "${RED}[ERROR]${NC} $1"
}

# Show usage
show_help() {
    cat << EOF
Usage: $0 [OPTIONS]

Build script for Distributed Cache Server

OPTIONS:
    -t, --type TYPE         Build type (Release, Debug, RelWithDebInfo) [default: Release]
    -d, --dir DIR          Build directory [default: build]
    -c, --clean            Clean build (remove build directory first)
    -j, --jobs N           Number of parallel jobs [default: $(nproc)]
    -T, --test             Run tests after build
    -i, --install          Install after build
    -v, --verbose          Verbose output
    -h, --help             Show this help message

EXAMPLES:
    $0                     # Standard release build
    $0 -t Debug -T         # Debug build with tests
    $0 -c -j 4             # Clean build with 4 parallel jobs
    $0 --clean --test --install  # Clean build, test, and install

EOF
}

# Parse command line arguments
while [[ $# -gt 0 ]]; do
    case $1 in
        -t|--type)
            BUILD_TYPE="$2"
            shift 2
            ;;
        -d|--dir)
            BUILD_DIR="$2"
            shift 2
            ;;
        -c|--clean)
            CLEAN_BUILD=true
            shift
            ;;
        -j|--jobs)
            JOBS="$2"
            shift 2
            ;;
        -T|--test)
            RUN_TESTS=true
            shift
            ;;
        -i|--install)
            INSTALL=true
            shift
            ;;
        -v|--verbose)
            VERBOSE=true
            shift
            ;;
        -h|--help)
            show_help
            exit 0
            ;;
        *)
            print_error "Unknown option: $1"
            show_help
            exit 1
            ;;
    esac
done

# Validate build type
if [[ ! "$BUILD_TYPE" =~ ^(Release|Debug|RelWithDebInfo|MinSizeRel)$ ]]; then
    print_error "Invalid build type: $BUILD_TYPE"
    print_info "Valid types: Release, Debug, RelWithDebInfo, MinSizeRel"
    exit 1
fi

# Check if we're in the project root
if [[ ! -f "CMakeLists.txt" ]]; then
    print_error "CMakeLists.txt not found. Please run this script from the project root."
    exit 1
fi

print_info "Starting build process..."
print_info "Build type: $BUILD_TYPE"
print_info "Build directory: $BUILD_DIR"
print_info "Parallel jobs: $JOBS"

# Check dependencies
print_info "Checking dependencies..."

check_dependency() {
    if ! command -v "$1" &> /dev/null; then
        print_error "$1 is not installed or not in PATH"
        return 1
    fi
}

# Check required tools
check_dependency "cmake" || exit 1
check_dependency "make" || exit 1

# Check compiler
if command -v "g++" &> /dev/null; then
    GCC_VERSION=$(g++ --version | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1)
    print_info "Found GCC version: $GCC_VERSION"
elif command -v "clang++" &> /dev/null; then
    CLANG_VERSION=$(clang++ --version | head -n1 | grep -oE '[0-9]+\.[0-9]+' | head -n1)
    print_info "Found Clang version: $CLANG_VERSION"
else
    print_error "No C++ compiler found (g++ or clang++)"
    exit 1
fi

# Clean build if requested
if [[ "$CLEAN_BUILD" == true ]]; then
    print_info "Cleaning build directory..."
    rm -rf "$BUILD_DIR"
fi

# Create build directory
print_info "Creating build directory..."
mkdir -p "$BUILD_DIR"
cd "$BUILD_DIR"

# Configure with CMake
print_info "Configuring with CMake..."
CMAKE_ARGS=(
    "-DCMAKE_BUILD_TYPE=$BUILD_TYPE"
    "-DCMAKE_EXPORT_COMPILE_COMMANDS=ON"
)

if [[ "$VERBOSE" == true ]]; then
    CMAKE_ARGS+=("-DCMAKE_VERBOSE_MAKEFILE=ON")
fi

cmake "${CMAKE_ARGS[@]}" .. || {
    print_error "CMake configuration failed"
    exit 1
}

# Build
print_info "Building with $JOBS parallel jobs..."
make -j"$JOBS" || {
    print_error "Build failed"
    exit 1
}

print_success "Build completed successfully!"

# Run tests if requested
if [[ "$RUN_TESTS" == true ]]; then
    print_info "Running tests..."
    if ! make test; then
        print_warning "Some tests failed"
        exit 1
    fi
    print_success "All tests passed!"
fi

# Install if requested
if [[ "$INSTALL" == true ]]; then
    print_info "Installing..."
    if [[ $EUID -eq 0 ]]; then
        make install
    else
        print_info "Installing with sudo..."
        sudo make install
    fi
    print_success "Installation completed!"
fi

# Show build summary
cd ..
print_success "Build summary:"
echo "  Build type: $BUILD_TYPE"
echo "  Build directory: $BUILD_DIR"
echo "  Binary location: $BUILD_DIR/cache_server"

if [[ -f "$BUILD_DIR/cache_server" ]]; then
    BINARY_SIZE=$(du -h "$BUILD_DIR/cache_server" | cut -f1)
    echo "  Binary size: $BINARY_SIZE"
fi

print_info "Build process completed successfully!"
print_info "You can now run the server with: ./$BUILD_DIR/cache_server --help"