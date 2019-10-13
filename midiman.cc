// cmidiman.cpp
// midi midi midi midi man
// wish life on thee 
#include <iostream>
#include <future>
#include <cstring>
#include <cstdlib>
#include "rtmidi/RtMidi.h"
#include <tinyalsa/asoundlib.h>
#include <signal.h>
#include <dirent.h>

struct sample {
  char *filename; 
  std::vector<char *> frames;
};

struct ctx {
    struct pcm *pcm;
    struct pcm_config config;
    struct sample *samples;
    unsigned int frameSize;
    unsigned int nSamples;
};

struct ctx ctx;
static int close = 0;
std::atomic_uchar tid;

void stream_close(int sig)
{
    /* allow the stream to be closed gracefully */
    signal(sig, SIG_IGN);
    close = 1;
}

int play_sample(int index, unsigned char thread_id)
{

    struct sample s = ctx.samples[index];
    unsigned int size = ctx.frameSize;
    unsigned int nFrames = s.frames.size();
    if (nFrames > 0){
      for (unsigned int i=0; i<nFrames; i++){
        if (tid.load() != thread_id){
        	break;
        }
        int num_write = pcm_writei(ctx.pcm, (char *)s.frames.at(i),
			pcm_bytes_to_frames(ctx.pcm, size));
	if (num_write < 0){
                fprintf(stderr, "error playing sample\n");
         }
      }
    }

    return 0;
}


void mycallback( double deltatime, std::vector< unsigned char > *message, void *userData )
{
  unsigned int nBytes = message->size();
  int offset = 24;
  if ( nBytes > 1 ){
    bool keyOn = (int)message->at(2) != 0;
    if (keyOn) {
      int note = (int)message->at(1);
      int sampleIndex = (note - offset) % ctx.nSamples;
      tid.store(tid.load()+1);
      fprintf(stdout, "%d,", tid.load());
      std::cout << "NOTE: "<<note <<" INDEX: "<<sampleIndex<< std::endl;
      std::thread t1(play_sample, sampleIndex, tid.load());
      t1.detach();
    } else {
      // stop sample on key up
      tid.store(tid.load()+1);
    }
  }
}

int ctx_init(struct ctx *ctx){
   ctx->config.period_size = 1024;
   ctx->config.period_count = 2;
   ctx->config.channels = 2;
   ctx->config.rate = 48000;
   ctx->config.format = PCM_FORMAT_S16_LE;
   ctx->config.silence_threshold = 1024 * 2;
   ctx->config.stop_threshold = 1024 * 2;
   ctx->config.start_threshold = 1024;


   // ctx->pcm = pcm_open(0, 0, PCM_OUT | PCM_MMAP, &ctx->config);
   ctx->pcm = pcm_open(0, 0, PCM_OUT, &ctx->config);
   if (ctx->pcm == NULL) {
       fprintf(stderr, "failed to allocate memory for pcm\n");
       return -1;
   } else if (!pcm_is_ready(ctx->pcm)) {
       fprintf(stderr, "failed to open for pcm %u,%u\n", 0, 0);
       pcm_close(ctx->pcm);
       return -1;
   }
   return 0;
}

// load samples from path into memory
int samples_init(struct ctx *ctx, std::string path){
 
  DIR *dir;
  struct dirent *ent;
  dir = opendir(path.c_str());
  std::vector<std::string> filenames;

  if (dir != NULL) {
    /* print all the files and directories within directory */
    do {
      ent = readdir(dir);
      if (ent != NULL){
	char *fname = ent->d_name;
	if (strstr(fname, ".wav") != NULL){
	   std::string p(path);
	   p.append(fname);
	   filenames.push_back(p);
    	   std::cout<<"READ: "<<p<<std::endl; 
	}
      }
    } while (ent != NULL);

    closedir (dir);
  } else {
    /* could not open directory */
    return  -1;
  }

  unsigned int nFiles = filenames.size();
  ctx->samples = new sample[nFiles];
  ctx->nSamples = nFiles;

  // load each sample file into memory 
  int bufsize = pcm_frames_to_bytes(ctx->pcm, pcm_get_buffer_size(ctx->pcm));
  for (unsigned int i=0; i<nFiles; i++){
    FILE *file = fopen(((std::string)filenames.at(i)).c_str(), "rb");
    if (file == NULL) {
       fprintf(stderr, "failed to open file %s\n", ((std::string)filenames.at(i)).c_str());
       return -1;
    }
    unsigned int num_read;
    do {
        char *buffer = new char[bufsize];
        if (!buffer) {
            fprintf(stderr, "unable to allocate %d bytes\n", bufsize);
            return -1;
        }
        num_read = fread(buffer, 1, bufsize, file);
        ctx->samples[i].frames.push_back( buffer );
    } while (!close && num_read > 0);
  }
  ctx->frameSize = bufsize;

  return 0;
}

int main()
{
 
  // open sound device
  if (ctx_init(&ctx) != 0){ 
      return -1;
  }
  std::string path("/home/pi/Music/samples/coolsnap/");
  if (samples_init(&ctx, path) != 0){
      return -1;
  }
  tid = 0;

  RtMidiIn *midiin = new RtMidiIn();
  // Check available ports.
  unsigned int nPorts = midiin->getPortCount();
  if ( nPorts == 0 ) {
    std::cout << "No ports available!\n";
    goto cleanup;
  }
  midiin->openPort( 0 );
  // Set our callback function.  This should be done immediately after
  // opening the port to avoid having incoming messages written to the
  // queue.
  midiin->setCallback( &mycallback );
  // Don't ignore sysex, timing, or active sensing messages.
  midiin->ignoreTypes( false, false, false );
  std::cout << "\nReading MIDI input ... press <enter> to quit.\n";
  char input;
  std::cin.get(input);
  // Clean up
 cleanup:
  delete midiin;
  return 0;
}

