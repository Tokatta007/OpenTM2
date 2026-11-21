/* Voice chat in the game: the microphone (push to talk or open mic), the speakers' voices played (voice_io.c). */
#ifndef TM2_NET_VOICE_IO_H
#define TM2_NET_VOICE_IO_H

typedef struct Lobby Lobby;
/* each frame (menus and matches): push_to_talk held now (-1: the server's voice chat is off); muted_machines (bits by
 * node) dropped. With no server (l NULL) only the mic test reads the microphone. */
void voice_io_frame(Lobby *l, int push_to_talk, unsigned muted_machines);
int voice_io_talking(Lobby *l, int node); /* that machine talking now (this PC's: sending) */
void voice_io_close(void);               /* the server left: every stream closed */
/* the Audio page: the mic test (the microphone read while on), its loudness now (0..1), the open mic's threshold on
 * the same scale, and the microphones there are */
void voice_io_mic_test(int on);
float voice_io_mic_level(void);
float voice_io_open_threshold(void);
int voice_io_mics(char names[][128], int max);

#endif
