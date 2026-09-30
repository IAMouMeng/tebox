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

import android.system.vold.CheckpointingState;
import android.system.vold.IVoldCheckpointListener;

/**
 * Vendor-available subset of android.os.IVold functionality.
 */
@VintfStability
interface IVold {
    /**
     * Register a checkpointing listener.
     *
     * @listener:
     *     listener to be added to the set of callbacks which will be invoked when checkpointing
     * completes or when Vold service knows that checkpointing will not be necessary.
     *
     * Return:
     *     CHECKPOINTING_COMPLETE when the listener will not be called in the future i.e. when
     * checkpointing is 1) already completed or 2) not needed.
     *     POSSIBLE_CHECKPOINTING when the listener will be called in the future, i.e. when
     * there is an active checkpoint or when service does not yet know whether checkpointing is
     * needed.
     */
    CheckpointingState registerCheckpointListener(IVoldCheckpointListener listener);
}
