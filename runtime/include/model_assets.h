#pragma once
// Graphics-owner lifetime. Metadata and identities remain owned until retirement;
// the backend reads the immutable GPU range directly into its resident storage.
// An explicit MH3_NATIVE_MODEL_PACK must exist. Without an override the optional
// disc-root/native-models/models.bin is used when present.
void LoadNativeModelAssets(const char* disc_root);
void UnloadNativeModelAssets();
