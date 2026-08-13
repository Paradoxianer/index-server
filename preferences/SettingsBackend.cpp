// Makefile-Engine's implicit rules get confused when a source file's
// basename matches the app's own NAME (here "IndexServerSettings" is both
// the preflet binary and server/IndexServerSettings.cpp's basename) - the
// object never gets compiled, the final link then fails with a missing .o.
// Compiling this single-TU wrapper under a different name instead avoids
// the clash while keeping server/IndexServerSettings.cpp the one real
// source of the class.
#include "../server/IndexServerSettings.cpp"
