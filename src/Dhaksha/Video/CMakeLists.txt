# ============================================================================
# Dhaksha fast video player
# Own RTSP client and the Android hardware decoder drawing straight onto the Fly View (see DhakshaNativePlayer.h)
# ============================================================================

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        DhakshaNativePlayer.cc
        DhakshaNativePlayer.h
        DhakshaRtspClient.cc
        DhakshaRtspClient.h
        DhakshaVideoItem.cc
        DhakshaVideoItem.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

if(ANDROID)
    target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE mediandk android)
endif()
