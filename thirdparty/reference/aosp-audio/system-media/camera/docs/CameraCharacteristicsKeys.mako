## -*- coding: utf-8 -*-
##
## Copyright (C) 2013 The Android Open Source Project
##
## Licensed under the Apache License, Version 2.0 (the "License");
## you may not use this file except in compliance with the License.
## You may obtain a copy of the License at
##
##      http://www.apache.org/licenses/LICENSE-2.0
##
## Unless required by applicable law or agreed to in writing, software
## distributed under the License is distributed on an "AS IS" BASIS,
## WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
## See the License for the specific language governing permissions and
## limitations under the License.
##
<%include file="CameraMetadataKeys.mako" args="java_class='CameraCharacteristics', xml_kind='static'" />
    /**
     * Mapping from INFO_SESSION_CONFIGURATION_QUERY_VERSION to session characteristics key.
     */
    private static final Map<Integer, Key<?>[]> AVAILABLE_SESSION_CHARACTERISTICS_KEYS_MAP =
            Map.ofEntries(
<% api_level_to_session_characteristic_keys = get_api_level_to_session_characteristic_keys(find_all_sections(metadata)) %>\
            %for idx, items in enumerate(api_level_to_session_characteristic_keys.items()):
<% api_level, keys = items %>\
                Map.entry(
                    ${api_level},
                    new Key<?>[] {
                    %for key in keys:
                        ${key | jkey_identifier},
                    %endfor
                    }
                )${',' if idx + 1 < len(api_level_to_session_characteristic_keys) else ''}
            %endfor
            );

    /*~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~
     * End generated code
     *~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~@~O@*/
