/* A stand-in PhysXLoader.dll for ../game_crash_test.sh: the PhysX system software is not part of the install, and
   without its loader Windows (and Wine) refuse to start SADK.exe. The test crashes the game before physics start, so
   the three imports SADK.exe has only need to exist. */
void *NxCreatePhysicsSDK(void) { return 0; }
void *NxGetPhysicsSDKAllocator(void) { return 0; }
void *NxGetCookingLib(void) { return 0; }
