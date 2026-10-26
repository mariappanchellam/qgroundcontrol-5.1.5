# ============================================================================
# Dhaksha Gimbal Camera
# Skydroid (Topotek protocol) and SIYI ZR10 camera control over the RC's network
# ============================================================================

target_sources(${CMAKE_PROJECT_NAME}
    PRIVATE
        DhakshaCameraManager.cc
        DhakshaCameraManager.h
        DhakshaGimbalCamera.cc
        DhakshaGimbalCamera.h
        SiyiGimbalCamera.cc
        SiyiGimbalCamera.h
        SkydroidGimbalCamera.cc
        SkydroidGimbalCamera.h
)

target_include_directories(${CMAKE_PROJECT_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
