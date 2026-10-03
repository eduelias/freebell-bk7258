# AVDK flavour of the doorbell firmware: the same app sources as project/, built inside Beken's
# bk_avdk tree so the multimedia (camera) components are available. Mounted at /avdk/projects/du7
# by scripts/build-avdk.sh, with project/main mounted at ./shared.
cmake_minimum_required(VERSION 3.5)
set(EXTRA_COMPONENTS_DIRS
    $ENV{ARMINO_PATH}/../components
    $ENV{ARMINO_PATH}/../components/bk_audio
    $ENV{ARMINO_PATH}/../components/bk_audio/audio_streams
    $ENV{ARMINO_PATH}/../components/codec/
    $ENV{ARMINO_PATH}/../components/codec/legacy
    $ENV{ARMINO_PATH}/../components/codec/legacy/media/audio
    $ENV{ARMINO_PATH}/../components/codec/legacy/media
    $ENV{ARMINO_PATH}/../components/bk_thirdparty/asr
    $ENV{ARMINO_PATH}/../components/bk_thirdparty
    $ENV{ARMINO_PATH}/../properties/modules
    $ENV{ARMINO_PATH}/../properties/modules/video_codec
    $ENV{ARMINO_PATH}/../properties/modules/audio_codec
    )
include($ENV{ARMINO_PATH}/tools/build_tools/cmake/project.cmake)
project(app)
