# Include the directory itself as a path to include directories
set (CMAKE_INCLUDE_CURRENT_DIR ON)

# Create a SOURCE variable containing all required .cpp files:
if (${USE_FLYCAP})
    set (oat-frameserve_SOURCE
         FrameServer.cpp
         TestFrame.cpp
         PointGreyCam.cpp
         WebCam.cpp
         FileReader.cpp)
else (${USE_FLYCAP})
    set (oat-frameserve_SOURCE
         FrameServer.cpp
         TestFrame.cpp
         WebCam.cpp
         FileReader.cpp)
endif (${USE_FLYCAP})

# Targets
add_executable (oat-frameserve ${oat-frameserve_SOURCE} main.cpp)
target_link_libraries (oat-frameserve
                       oat-utility
                       oat-base
                       ${OatCommon_LIBS}
                       ${FLYCAPTURE2})
add_dependencies (oat-frameserve cpptoml)

# Installation
install (TARGETS oat-frameserve DESTINATION ../../oat/libexec COMPONENT oat-processors)
