/*
 * Copyright (C) 2024 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

package android.system.vold;

/**
 * Describes the IVold service's possible checkpointing states.
 */
@VintfStability
enum CheckpointingState {
    /**
     * The service has not yet determined whether checkpointing is needed, or there is a checkpoint
     * active.
     */
    POSSIBLE_CHECKPOINTING,
    /**
     * There is no checkpoint active and there will not be any more checkpoints before the system
     * reboots.
     */
    CHECKPOINTING_COMPLETE,
}