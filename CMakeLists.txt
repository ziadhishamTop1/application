cmake_minimum_required(VERSION 3.20)
project(WebSocketApp)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Set vcpkg toolchain
set(CMAKE_TOOLCHAIN_FILE "C:/vcpkg/scripts/buildsystems/vcpkg.cmake" CACHE STRING "Vcpkg toolchain file")

# Include vcpkg dependencies
include_directories("C:/vcpkg/installed/x64-windows/include")
link_directories("C:/vcpkg/installed/x64-windows/lib")

# Set policy CMP0167 to NEW
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 NEW)
endif()

# Find Boost (add required components)
find_package(Boost REQUIRED COMPONENTS system asio beast json serialization container)

# Find CURL
find_package(CURL REQUIRED)

# Find libwebsockets
find_package(libwebsockets REQUIRED)

# Find simdjson
find_package(simdjson REQUIRED)

# Find OpenSSL
find_package(OpenSSL REQUIRED)

# Find TBB
find_package(TBB REQUIRED)

# Find FlatBuffers
find_package(flatbuffers CONFIG REQUIRED)

# Include directories
include_directories(
    ${CMAKE_SOURCE_DIR}/include
    ${Boost_INCLUDE_DIRS}
    ${OPENSSL_INCLUDE_DIR}
    ${CURL_INCLUDE_DIRS}
    ${LIBWEBSOCKETS_INCLUDE_DIRS}
    ${TBB_INCLUDE_DIRS}
)

# Set LIBUS_EXT_ALIGNMENT to 64
add_definitions(-DLIBUS_EXT_ALIGNMENT=64)


# Add executables
#add_executable(WebSocketAppReader "src/fuckAroundAVX.cpp" "src/executer.cpp")
add_executable(Algorithm  "main.cpp" "src/executer.cpp" )
add_executable(Client "src/Client.cpp")

# Link libraries to WebSocketAppReader

# Link libraries to WebSocketAppReader
target_link_libraries(Algorithm PRIVATE
    Boost::system
    Boost::asio
    Boost::beast
    Boost::json
    Boost::container  # Provides boost::circular_buffer
    Boost::serialization
    simdjson::simdjson
    flatbuffers::flatbuffers
    websockets
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    TBB::tbb
    ws2_32
)

# Link libraries to WebSocketAppReader
target_link_libraries(Client PRIVATE
    Boost::system
    Boost::asio
    Boost::beast
    Boost::json
    Boost::container  # Provides boost::circular_buffer
    Boost::serialization
    simdjson::simdjson
    flatbuffers::flatbuffers
    websockets
    OpenSSL::SSL
    OpenSSL::Crypto
    CURL::libcurl
    TBB::tbb
    ws2_32
)


# Define Windows 10/11 target
target_compile_definitions(Algorithm PRIVATE
    _WIN32_WINNT=0x0A00  # Windows 10/11
)

target_compile_definitions(Client PRIVATE
   _WIN32_WINNT=0x0A00  # Windows 10/11
)

if(WIN32)
    add_definitions(
        -DNOMINMAX
        -D_WINSOCK_DEPRECATED_NO_WARNINGS
        -D_WINSOCKAPI_
    )
endif()

# MSVC-specific linker options
if (MSVC)
    add_compile_options(/arch:AVX2)
    target_link_options(Algorithm PRIVATE /NODEFAULTLIB:MSVCRT)
    target_link_options(Client PRIVATE /NODEFAULTLIB:MSVCRT)
endif()

