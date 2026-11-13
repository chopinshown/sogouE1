/* Configure only the two E1 bridge cables; leave default device routing alone.
 */
#include <CoreAudio/CoreAudio.h>
#include <CoreFoundation/CoreFoundation.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char **argv) {
  int configure = argc == 2 && !strcmp(argv[1], "--configure");
  if (argc > 1 && !configure) {
    fprintf(stderr, "Usage: mac-audio-setup [--configure]\n");
    return 2;
  }
  AudioObjectPropertyAddress property = {kAudioHardwarePropertyDevices,
                                         kAudioObjectPropertyScopeGlobal,
                                         kAudioObjectPropertyElementMain};
  UInt32 size = 0;
  if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &property, 0,
                                     NULL, &size))
    return 1;
  AudioDeviceID *devices = malloc(size);
  if (!devices)
    return 1;
  if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &property, 0, NULL,
                                 &size, devices)) {
    free(devices);
    return 1;
  }
  unsigned found = 0;
  int failed = 0;
  for (unsigned i = 0; i < size / sizeof *devices; ++i) {
    AudioObjectPropertyAddress name_property = {
        kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    CFStringRef name = NULL;
    UInt32 n = sizeof name;
    if (AudioObjectGetPropertyData(devices[i], &name_property, 0, NULL, &n,
                                   &name))
      continue;
    char text[256];
    int valid =
        CFStringGetCString(name, text, sizeof text, kCFStringEncodingUTF8);
    CFRelease(name);
    if (!valid ||
        (strcmp(text, "BlackHole 2ch") && strcmp(text, "BlackHole 16ch")))
      continue;
    ++found;
    AudioObjectPropertyAddress rate_property = {
        kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal,
        kAudioObjectPropertyElementMain};
    Float64 rate = 0;
    n = sizeof rate;
    OSStatus error = AudioObjectGetPropertyData(devices[i], &rate_property, 0,
                                                NULL, &n, &rate);
    if (configure && !error && rate != 44100) {
      Float64 requested = 44100;
      error = AudioObjectSetPropertyData(devices[i], &rate_property, 0, NULL,
                                         sizeof requested, &requested);
      for (unsigned attempt = 0; !error && attempt < 200; ++attempt) {
        n = sizeof rate;
        error = AudioObjectGetPropertyData(devices[i], &rate_property, 0, NULL,
                                           &n, &rate);
        if (error || rate == 44100)
          break;
        usleep(10000);
      }
    }
    printf("%s: %.0f Hz%s\n", text, rate,
           error ? " (configuration failed)" : "");
    if (error || (configure && rate != 44100))
      failed = 1;
  }
  free(devices);
  if (found != 2) {
    fprintf(stderr,
            "Install BlackHole 2ch and 16ch, then reload Core Audio.\n");
    return 1;
  }
  return failed;
}
