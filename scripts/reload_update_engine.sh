#
# Copyright (C) 2025 The Android Open Source Project
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#      http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.
#
#!/usr/bin/zsh
#
#
# ####################################################################
# Script Name:  reload_update_engine.sh
# Description:  replaces on device update_engine daemon with host copy 
# Author:       Kelvin Zhang
# Date:         2025-08-19
# Version:      1.0
#
# Usage:        ./reload_update_engine.sh
#
#
# Notes:
#   - Use for quick testing of update_engine changes without reflashing
#     or reconfiguring the entire device
# ####################################################################
set -v
set -e
adb root
adb wait-for-device
# adb shell update_engine_client --reset_status || echo
# adb shell rm -rf /data/misc/update_engine/prefs || echo
adb shell killall update_engine || echo
adb shell stop update_engine
adb push $ANDROID_PRODUCT_OUT/system/bin/update_engine /data/
adb shell LD_LIBRARY_PATH=/data /data/update_engine --logtostderr
mkdir -p $OUT/symbols/data
ln -fs $OUT/symbols/system/bin/update_engine $OUT/symbols/data/
