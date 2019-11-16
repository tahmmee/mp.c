#include <stdexcept>
#include <stdio.h>
#include <string.h>
#include <future>
#include <time.h>
#include <vector>
#include <dirent.h>
#include "RunParameters.h"
#include "WavFile.h"
#include <soundtouch/SoundTouch.h>
#include <soundtouch/BPMDetect.h>
#include <alsa/asoundlib.h>

using namespace soundtouch;
using namespace std;

// Processing chunk size (size chosen to be divisible by 2, 4, 6, 8, 10, 12, 14, 16 channels ...)
#define BUFF_SIZE  1024*2
#define PCM_DEVICE "default"
#define RATE 44100
#define TEMPO_CTL 0x12 
#define PITCH_CTL 0x13
//#define RATE_CTL 0x56
#define RATE_CTL 0x1
#define SLICE_START_CTL 0x51
#define SLICE_END_CTL 0x5b
#define MODE_CTL 0x50
#define SUPER_LOW_KEY 24
#define CHANNELS 2
#define SOUNDTOUCH_INTEGER_SAMPLES 1
#define SET_STREAM_TO_BIN_MODE(f) {}
#define MAX_PCM_HANDLES 5
#define MAX_SAMPLES 16 
#define MAX_SLICES 88 


struct slice {
	long unsigned int start;
	long unsigned int start_offset;
	long unsigned int end;
	long unsigned int end_offset;
};

struct sample {
  unsigned int channels;
  unsigned int rate;
  int bits;
	// lowest key in range
	int low_key;
	vector<SAMPLETYPE*> *buffers;
  WavInFile *file;
	slice slices[MAX_SLICES];
	slice *selectedSlice;
  int bpm;
};

enum fx_mode{ST_STRETCH, ST_PASSTHROUGH= 0x19, ST_MACHINE=0x33};
struct fxctl {
  fx_mode mode;
  int pitch;
  int rate;
};

// wrapper for pcm type 
struct pcm_handle {
	snd_pcm_t *pcm;
};

// enum chp_program{CHP_BROWSE, CHP_EDIT = 0x19, CHP_MPC = 0x33};
enum chp_program{CHP_BROWSE, CHP_EDIT, CHP_MPC};
struct ctx {

  // program
  chp_program prog;

  // pcm_outputs
  pcm_handle pcm_handles[MAX_PCM_HANDLES];

  // cursor of pcms
	unsigned int pcm_cursor;

  // sample browser
  vector<sample *> snippets;

  // active samples 
  sample samples[MAX_SAMPLES];

  // current midi chan
  int midi_chan;

  // selected sample to edit
	sample *selectedSample;

  // modulation 
  SoundTouch soundTouch;

  //fxctl 
  fxctl fx;

  // midi 
  snd_seq_t *seq_handle;

};

// thread id running
atomic_uchar tid[MAX_SAMPLES];

static const char _helloText[] = 
"\n"
"   Chopage v%s -  Copyright (c) Dichtomas Monk\n"
"=========================================================\n";

// Open all files and store frames in buffer
static int openFiles(WavInFile **inFile, struct ctx *ctx, const RunParameters *params)
{

  // open snippets...
  DIR *dir;
  struct dirent *ent;
  char *path = params->samplePath;
  dir = opendir(path);
  if (dir == NULL) {
    fprintf(stderr, "Unable to open dir %s\n", path);
    return -1;
  }
  do {
    ent = readdir(dir);
    if (ent != NULL){
      char *fname = ent->d_name;
      if (strstr(fname, ".wav") != NULL){
        string p(path);
        p.append(fname);
        WavInFile *wf = new WavInFile(p.c_str());
        
        // init sample 
        struct sample *s = new sample();
        s->file = wf;
        s->buffers = new vector<SAMPLETYPE*>(0);
				s->low_key = 0;

        // read preview frames to memory
        // TODO: make it a slice
        int previewCount = 100;
        while ((wf->eof() == 0) && (previewCount-- > 0)){
          int num;
          SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
          num = wf->read(buff, BUFF_SIZE);
          s->buffers->push_back(buff);

        }

        printf("Read %s\n", p.c_str());

        // add sample to context
        ctx->snippets.push_back(s);
      }
    }
  } while (ent != NULL);

  closedir (dir);
  return 0;
}

// re-populate the sample buffers
void loadSelectedSnippet(ctx *ctx){

  // get selected snippet
  sample *s;
	if (ctx->selectedSample != NULL){
		s = ctx->selectedSample;
  } else if (ctx->snippets.size() > 0){
		s = ctx->snippets.at(0);
	} else {
	 	fprintf (stderr, "Could not select a sample to load\n");
		return;
	}

  // prepare sample
  if (s->buffers != NULL && s->buffers->size() > 0){
    delete s->buffers;
  }
  s->buffers = new vector<SAMPLETYPE*>(0);
	s->file->rewind();

  // unload sample currently loaded
  // for selected channel 
  sample *chan_sample = &ctx->samples[ctx->midi_chan];
  if ( chan_sample->buffers != NULL){
    if (chan_sample->buffers->size() > 0){
      delete chan_sample->buffers;
    }
    // TODO: rm slices
    //for (int i = 0; i < MAX_SLICES; i++){
    //  chan_sample[i]->start = 0;
    //}
  }


  // init bpm analyzer
  int nChannels = (int)s->file->getNumChannels();
  BPMDetect bpm(nChannels, s->file->getSampleRate());
  
  while (s->file->eof() == 0){
    int num;
    SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
    num = s->file->read(buff, BUFF_SIZE);
    s->buffers->push_back(buff);

    // Enter the new samples to the bpm analyzer class
    bpm.inputSamples(buff, num / nChannels);
  }

  s->bpm = bpm.getBpm();
  printf("Loaded on Ch:%d (%d bpm)\n",ctx->midi_chan, s->bpm);
  ctx->samples[ctx->midi_chan] = *s;

  // set sample tempo to 120
  // int tempoDelta = (120 / s->bpm - 1.0f) * 100.0f;
  // printf("TEMPO DELLIETA! %d -> %d\n", s->bpm , tempoDelta);
  //ctx->soundTouch.setTempoChange(tempoDelta);
}


static snd_pcm_t* initPCM(snd_pcm_stream_t stream){

  // pcm init
  unsigned int pcm;
  snd_pcm_hw_params_t *params;
  snd_pcm_t *pcm_handle;

	if (pcm = snd_pcm_open(&pcm_handle, PCM_DEVICE,
				stream, 0) < 0)
		printf("ERROR: Can't open \"%s\" PCM. %s\n",
				stream, snd_strerror(pcm));

  // aloc params with default values
  snd_pcm_hw_params_alloca(&params);
  snd_pcm_hw_params_any(pcm_handle, params);

  // override defaults
  if (pcm = snd_pcm_hw_params_set_format(pcm_handle, params,
        SND_PCM_FORMAT_FLOAT_LE) < 0)
    printf("ERROR: Can't set format. %s\n", snd_strerror(pcm));

  if (pcm =  snd_pcm_hw_params_set_access(pcm_handle, params,
        SND_PCM_ACCESS_RW_INTERLEAVED) < 0)
    printf("ERROR: Can't set access. %s\n", snd_strerror(pcm));

  if (pcm = snd_pcm_hw_params_set_channels(pcm_handle, params, CHANNELS) < 0) 
    printf("ERROR: Can't set channels number. %s\n", snd_strerror(pcm));

  unsigned int rate = RATE;
  if (pcm = snd_pcm_hw_params_set_rate_near(pcm_handle, params, &rate, 0) < 0) 
    printf("ERROR: Can't set rate. %s\n", snd_strerror(pcm));


  if (pcm = snd_pcm_hw_params_set_buffer_size(pcm_handle, params, BUFF_SIZE/2) < 0) 
    printf("ERROR: Can't set buffersize. %s\n", snd_strerror(pcm));

  unsigned int period = 2; 
  if (pcm = snd_pcm_hw_params_set_period_time_near(pcm_handle, params, &period, 0) < 0) 
    printf("ERROR: Can't set period time. %s\n", snd_strerror(pcm));

  /* Write parameters */
  if (pcm = snd_pcm_hw_params(pcm_handle, params) < 0)
    printf("ERROR: Can't set harware parameters. %s\n", snd_strerror(pcm));

  snd_pcm_uframes_t   	buffer_size; 
  snd_pcm_uframes_t   	period_size;
  snd_pcm_get_params(pcm_handle, &buffer_size, &period_size);
  printf("PCM OPENED WITH: %ld buffer, %ld period\n", buffer_size, period_size);

	if ((pcm = snd_pcm_prepare (pcm_handle)) < 0) {
	 	fprintf (stderr, "cannot prepare audio interface for use (%s)\n",
	 					 snd_strerror (pcm));
	 	exit (1);
	}

  return pcm_handle;
}

// Sets the 'SoundTouch' object up according to input file sound format & 
// command line parameters
static void setup(SoundTouch *pSoundTouch, const RunParameters *params)
{
  int sampleRate;
  int channels;

  pSoundTouch->setSampleRate(RATE);
  pSoundTouch->setChannels(CHANNELS);

  pSoundTouch->setTempoChange(params->tempoDelta);
  pSoundTouch->setPitchSemiTones(params->pitchDelta);
  pSoundTouch->setRateChange(params->rateDelta);

  pSoundTouch->setSetting(SETTING_USE_QUICKSEEK, params->quick);
  pSoundTouch->setSetting(SETTING_USE_AA_FILTER, !(params->noAntiAlias));

  if (params->speech)
  {
    // use settings for speech processing
    pSoundTouch->setSetting(SETTING_SEQUENCE_MS, 40);
    pSoundTouch->setSetting(SETTING_SEEKWINDOW_MS, 15);
    pSoundTouch->setSetting(SETTING_OVERLAP_MS, 8);
    fprintf(stderr, "Tune processing parameters for speech processing.\n");
  }

  fflush(stderr);
}

void openMidi(struct ctx *ctx)
{
  snd_seq_open(&ctx->seq_handle, "default", SND_SEQ_OPEN_INPUT, 0);

  snd_seq_set_client_name(ctx->seq_handle, "Choppage");
  int port = snd_seq_create_simple_port(ctx->seq_handle, "Choppage Input",
      SND_SEQ_PORT_CAP_WRITE|SND_SEQ_PORT_CAP_SUBS_WRITE,
      SND_SEQ_PORT_TYPE_APPLICATION);
}

static snd_pcm_t* pcm_handle(ctx *ctx) {
	ctx->pcm_cursor = (ctx->pcm_cursor+1) % MAX_PCM_HANDLES;
	return ctx->pcm_handles[ctx->pcm_cursor].pcm;
}

// pass in the interval 
static void play_sample(ctx *ctx, slice *slc, unsigned char thread_id)
{

  int err, nSamples;
	long unsigned int start, end;

	sample *s = ctx->selectedSample;
  unsigned int nBuffers = s->buffers->size();
  SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];

	if (slc == NULL){
		start = 0;
		end = nBuffers;
	} else {
		start = slc->start + slc->start_offset;
		if (ctx->prog == CHP_EDIT){
			// play to end in edit mode because this can be changed
			end = nBuffers;
		}
		if (ctx->prog == CHP_MPC){
			end = slc->end + slc->end_offset;
		}
	}

	snd_pcm_t *pcm = pcm_handle(ctx);
  snd_pcm_prepare(pcm);

  for (long unsigned int i=start; i<end; i++){
    unsigned char diff = tid[ctx->midi_chan].load() - thread_id;
    printf("DIFF %d\n", diff);
    if (diff != 0) {
      // stopping
     	snd_pcm_drop(pcm);
  		snd_pcm_prepare(pcm);
			// update slice end
			if ((slc != NULL) && (ctx->prog == CHP_EDIT)) {
				slc->end = i-3;
			}
      return;  
    }

    nSamples = BUFF_SIZE/CHANNELS;
    memcpy(buff, s->buffers->at(i), BUFF_SIZE*4);

    // Feed the samples into SoundTouch processor
    ctx->soundTouch.putSamples(buff, nSamples);
    nSamples = ctx->soundTouch.receiveSamples(buff, nSamples);

    // output
   if ((err = snd_pcm_writei(pcm, buff, nSamples)) < 0){
     printf("write err %s\n", snd_strerror(err));
   }
  }
}

// Play just a slice of sample
static void play_slice(ctx *ctx, int note, unsigned char thread_id)
{

  int pcm, nSamples;
	sample *s = ctx->selectedSample;
  unsigned int nBuffers = s->buffers->size();
  SAMPLETYPE *buff = new SAMPLETYPE[BUFF_SIZE];
	
  // get slice associated with note
	slice *slc = &ctx->selectedSample->slices[note];
	// if slice interval is not set then start at
	// end of closest note with lower interval
	long unsigned int startPos = slc->start;
	if ((startPos == 0) && (note > s->low_key)) {
		for (int i = note; i > 0; i--){
			startPos = ctx->selectedSample->slices[i].end;
			if (startPos > 0){
				slc->start = startPos;
				break;
			}
		}
	}

	// setting as low key since no previous key has interval
	if ((startPos == 0) && (s->low_key == 0)) {
		s->low_key = note;
	}

	s->selectedSlice = slc;
	play_sample(ctx, slc, thread_id);
}


snd_seq_event_t *readMidi(struct ctx *ctx)
{
  snd_seq_event_t *ev = NULL;
  int err;
	if ((err = snd_seq_event_input(ctx->seq_handle, &ev)) < 0){
		if (err != -EAGAIN){
    	printf("midi event err: %s\n", snd_strerror(err));
		}
		return NULL;
  }

  if ((ev->type == SND_SEQ_EVENT_NOTEON)||(ev->type == SND_SEQ_EVENT_NOTEOFF)) {
    const char *type = (ev->type == SND_SEQ_EVENT_NOTEON) ? "on " : "off";
    printf("[%d] Note %s: %2x vel(%2x)\n", ev->data.note.channel, type,
        ev->data.note.note,
        ev->data.note.velocity);
    ctx->midi_chan = ev->data.note.channel;

    if ((ev->type == SND_SEQ_EVENT_NOTEON) && (ev->data.note.velocity > 0)){
	
			// update thread id
			// this will also stop other running samples
      tid[ctx->midi_chan].store(tid[ctx->midi_chan].load()+1);
      ctx->soundTouch.clear();

			// play sample based on program
			if (ctx->prog == CHP_BROWSE){

			  // no matter how many notes we have
				// sample will be spread across them all
				int index = ev->data.note.note % ctx->snippets.size();
  			// update selected sample 
  			ctx->selectedSample = ctx->snippets.at(index);
				thread t1(play_sample, ctx, nullptr, tid[ctx->midi_chan].load());
				t1.detach();
			}
			if ((ctx->prog == CHP_EDIT) || (ctx->prog == CHP_MPC)){
  			// update selected for channel 
  			ctx->selectedSample = &ctx->samples[ctx->midi_chan];
        // do not try and unloaded sample
        if (ctx->selectedSample->buffers == NULL){
          printf("No slices on channel %d\n", ctx->midi_chan);
          return NULL;
        }

				// play slice
				int note = ev->data.note.note;
				thread t1(play_slice, ctx, note, tid[ctx->midi_chan].load());
				t1.detach();
			}
    } else {
      // stop sample on key up when not in mpc mode
			if (ctx->prog != CHP_MPC){
      	tid[ctx->midi_chan].store(tid[ctx->midi_chan].load()+1);
			}
    }
  } else if (ev->type == SND_SEQ_EVENT_PGMCHANGE) {
    printf("Program Change:  %2x \n", ev->data.control.value);
		if (ev->data.control.value == CHP_EDIT){
			ctx->prog = CHP_EDIT;
			loadSelectedSnippet(ctx);
		}
		if (ev->data.control.value == CHP_MPC){
			ctx->prog = CHP_MPC;
		}
		if (ev->data.control.value == CHP_BROWSE){
			ctx->prog = CHP_BROWSE;
			// TODO: unload selected sample
		}
  } else if(ev->type == SND_SEQ_EVENT_CONTROLLER) {
    printf("Control:  %2x val(%2x)\n", ev->data.control.param,
        ev->data.control.value);


		// set prog mode
    if(ev->data.control.param == MODE_CTL){
			if (ev->data.control.value == CHP_EDIT){
				ctx->prog = CHP_EDIT;
				loadSelectedSnippet(ctx);
			}
			if (ev->data.control.value == CHP_MPC){
				ctx->prog = CHP_MPC;
			}
			if (ev->data.control.value == CHP_BROWSE){
				ctx->prog = CHP_BROWSE;
				// TODO: unload selected sample
			}
    }

		// start slice editor
		if (ev->data.control.param == SLICE_START_CTL){
			if (ctx->selectedSample != NULL){
				if (ctx->selectedSample->selectedSlice != NULL){
					ctx->selectedSample->selectedSlice->start_offset = ev->data.control.value - 64;
				}
			}
		}
		// end slice editor
		if (ev->data.control.param == SLICE_END_CTL){
			if (ctx->selectedSample != NULL){
				if (ctx->selectedSample->selectedSlice != NULL){
					ctx->selectedSample->selectedSlice->end_offset = ev->data.control.value - 64;
				}
			}
		}

		// adjust fx

    // change tempo at same pitch
    if(ev->data.control.param == TEMPO_CTL){
      int tempo = ev->data.control.value - 64;
      printf("Tempo: %d\n", tempo);
      ctx->soundTouch.setTempoChange(tempo);
    }

    // change pitch	at same tempo 
    if(ev->data.control.param == PITCH_CTL){
      int pitch = ev->data.control.value/4 - 16;
      printf("Pitch: %d\n", pitch);
      ctx->soundTouch.setPitchSemiTones(pitch);
    }

		// change both tempo and pitch
    if(ev->data.control.param == RATE_CTL){
      int rate = ev->data.control.value - 64;
      ctx->soundTouch.setRateChange(rate);
      printf("Rate: %d\n", rate);
    }



  } else if (ev->type == SND_SEQ_EVENT_PORT_SUBSCRIBED){
    printf("Connected to midi controller\n");
  } else if (ev->type == SND_SEQ_EVENT_SENSING){
    // ignore
  } else if (ev->type == SND_SEQ_EVENT_PITCHBEND) {
    printf("Pitch Bend %d\n", ev->data.control.value);
  } else if (ev != NULL) {
    printf("[%d] Unknown:  Unhandled Event Received %d\n", ev->time.tick, ev->type);
	} else {
		printf("Unkown error");
  }
  return ev;
}




// Detect BPM rate of inFile and adjust tempo setting accordingly if necessary
static void detectBPM(WavInFile *inFile, RunParameters *params)
{
  float bpmValue;
  int nChannels;
  BPMDetect bpm(inFile->getNumChannels(), inFile->getSampleRate());
  SAMPLETYPE sampleBuffer[BUFF_SIZE];

  // detect bpm rate
  fprintf(stderr, "Detecting BPM rate...");
  fflush(stderr);

  nChannels = (int)inFile->getNumChannels();

  // Process the 'inFile' in small blocks, repeat until whole file has 
  // been processed
  while (inFile->eof() == 0)
  {
    int num, samples;

    // Read sample data from input file
    num = inFile->read(sampleBuffer, BUFF_SIZE);

    // Enter the new samples to the bpm analyzer class
    samples = num / nChannels;
    bpm.inputSamples(sampleBuffer, samples);
  }

  // Now the whole song data has been analyzed. Read the resulting bpm.
  bpmValue = bpm.getBpm();
  fprintf(stderr, "Done!\n");

  // rewind the file after bpm detection
  inFile->rewind();

  if (bpmValue > 0)
  {
    fprintf(stderr, "Detected BPM rate %.1f\n\n", bpmValue);
  }
  else
  {
    fprintf(stderr, "Couldn't detect BPM rate.\n\n");
    return;
  }

  if (params->goalBPM > 0)
  {
    // adjust tempo to given bpm
    params->tempoDelta = (params->goalBPM / bpmValue - 1.0f) * 100.0f;
    fprintf(stderr, "The file will be converted to %.1f BPM\n\n", params->goalBPM);
  }
}


int main(const int nParams, const char * const paramStr[])
{
  WavInFile *inFile;
  RunParameters *params;
  struct ctx ctx;
  ctx.prog = CHP_BROWSE;

  fprintf(stderr, _helloText, SoundTouch::getVersionString());

  try 
  {
    // Parse command line parameters
    params = new RunParameters(nParams, paramStr);

		// Open pcm handles 
		for (int i = 0; i < MAX_PCM_HANDLES; i++){
			ctx.pcm_handles[i].pcm = initPCM(SND_PCM_STREAM_PLAYBACK);
		 if (ctx.pcm_handles[i].pcm == 0)
			 return -1;
		}
		ctx.pcm_cursor = 0;


		// open Midi port
    openMidi(&ctx);

    // Open input samples
    if (openFiles(&inFile, &ctx, params) != 0)
      return -1;

    // Setup the 'SoundTouch' object for processing the sound
    setup(&ctx.soundTouch, params);

    // Run controller 
    while (1) {
      readMidi(&ctx);
    }

    // Close WAV file handles & dispose of the objects
    // TODO: cleanup
    // delete inFile;
    delete params;

    fprintf(stderr, "Done!\n");
  } 
  catch (const runtime_error &e) 
  {
    // An exception occurred during processing, display an error message
    fprintf(stderr, "%s\n", e.what());
    return -1;
  }

  return 0;
}
