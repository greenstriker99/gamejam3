//Julio Huerta
//3/30/21
//Senior Project - Gamejam Game 3 Tower Attack


#include <stdlib.h>
#include <string.h>

// include NESLIB header
#include "neslib.h"

// include CC65 NES Header (PPU)
#include <nes.h>

// BCD arithmetic support
#include "bcd.h"
//#link "bcd.c"

// VRAM update buffer
#include "vrambuf.h"
//#link "vrambuf.c"

// link the pattern table into CHR ROM
//#link "chr_generic.s"

// famitone2 library
//#link "famitone2.s"

// music and sfx
//#link "music_dangerstreets.s"
extern char danger_streets_music_data[];
//#link "demosounds.s"
extern char demo_sounds[];

// indices of sound effects (0..3)
typedef enum { SND_START, SND_HIT, SND_COIN, SND_JUMP } SFXIndex;

///// DEFINES

#define COLS 30		// floor width in tiles
#define ROWS 60		// total scrollable height in tiles

#define MAX_FLOORS 25		// total # of floors in a stage
#define GAPSIZE 4		// gap size in tiles
#define BOTTOM_FLOOR_Y 2	// offset for bottommost floor

#define MAX_ACTORS 5		// max # of moving actors
#define SCREEN_Y_BOTTOM 208	// bottom of screen in pixels
#define ACTOR_MIN_X 16		// leftmost position of actor
#define ACTOR_MAX_X 228		// rightmost position of actor
#define ACTOR_SCROLL_UP_Y 110	// min Y position to scroll up
#define ACTOR_SCROLL_DOWN_Y 140	// max Y position to scroll down
#define JUMP_VELOCITY 20	// Y velocity when jumping

// constants for various tiles
#define CH_BORDER 0x40
#define CH_FLOOR 0xf4
#define CH_ITEM 0xc4
#define CH_BLANK 0x20
#define CH_BASEMENT 0x97

///// GLOBALS

// vertical scroll amount in pixels
static int scroll_pixel_yy = 0;

// vertical scroll amount in tiles (scroll_pixel_yy / 8)
static byte scroll_tile_y = 0;

// last screen Y position of player sprite
static byte player_screen_y = 0;

// score (BCD)
static byte score = 0;
static char hit = 0;

// screen flash animation (virtual bright)
static byte vbright = 4;

// random byte between (a ... b-1)
// use rand() because rand8() has a cycle of 255
byte rndint(byte a, byte b) {
  return (rand() % (b-a)) + a;
}

// return nametable address for tile (x,y)
// assuming vertical scrolling (horiz. mirroring)
word getntaddr(byte x, byte y) {
  word addr;
  if (y < 30) {
    addr = NTADR_A(x,y);	// nametable A
  } else {
    addr = NTADR_C(x,y-30);	// nametable C
  }
  return addr;
}

// convert nametable address to attribute address
word nt2attraddr(word a) {
  return (a & 0x2c00) | 0x3c0 |
    ((a >> 4) & 0x38) | ((a >> 2) & 0x07);
}

/// METASPRITES

// define a 2x2 metasprite
#define DEF_METASPRITE_2x2(name,code,pal)\
const unsigned char name[]={\
        0,      0,      (code)+0,   pal, \
        0,      8,      (code)+1,   pal, \
        8,      0,      (code)+2,   pal, \
        8,      8,      (code)+3,   pal, \
        128};

// define a 2x2 metasprite, flipped horizontally
#define DEF_METASPRITE_2x2_FLIP(name,code,pal)\
const unsigned char name[]={\
        8,      0,      (code)+0,   (pal)|OAM_FLIP_H, \
        8,      8,      (code)+1,   (pal)|OAM_FLIP_H, \
        0,      0,      (code)+2,   (pal)|OAM_FLIP_H, \
        0,      8,      (code)+3,   (pal)|OAM_FLIP_H, \
        128};

// right-facing
DEF_METASPRITE_2x2(playerRStand, 0xd8, 0);
DEF_METASPRITE_2x2(playerRRun1, 0xdc, 0);
DEF_METASPRITE_2x2(playerRRun2, 0xe0, 0);
DEF_METASPRITE_2x2(playerRRun3, 0xe4, 0);
DEF_METASPRITE_2x2(playerRJump, 0xe8, 0);
DEF_METASPRITE_2x2(playerRClimb, 0xec, 0);
DEF_METASPRITE_2x2(playerRSad, 0xf0, 0);

// left-facing
DEF_METASPRITE_2x2_FLIP(playerLStand, 0xd8, 0);
DEF_METASPRITE_2x2_FLIP(playerLRun1, 0xdc, 0);
DEF_METASPRITE_2x2_FLIP(playerLRun2, 0xe0, 0);
DEF_METASPRITE_2x2_FLIP(playerLRun3, 0xe4, 0);
DEF_METASPRITE_2x2_FLIP(playerLJump, 0xe8, 0);
DEF_METASPRITE_2x2_FLIP(playerLClimb, 0xec, 0);
DEF_METASPRITE_2x2_FLIP(playerLSad, 0xf0, 0);

// player run sequence
const unsigned char* const playerRunSeq[16] = {
  playerLRun1, playerLRun2, playerLRun3, 
  playerLRun1, playerLRun2, playerLRun3, 
  playerLRun1, playerLRun2,
  playerRRun1, playerRRun2, playerRRun3, 
  playerRRun1, playerRRun2, playerRRun3, 
  playerRRun1, playerRRun2,
};

///// GAME LOGIC

// struct definition for a single floor
typedef struct Floor {
  byte ypos;		// # of tiles from ground
  int height:4;		// # of tiles to next floor
  int gap:4;		// X position of gap
  int ladder1:4;	// X position of first ladder
  int ladder2:4;	// X position of second ladder
  int objtype:4;	// item type (FloorItem)
  int objpos:4;		// X position of object
} Floor;

// various items the player can pick up
typedef enum FloorItem { ITEM_NONE, ITEM_MINE, ITEM_HEART, ITEM_POWER };

// array of floors
Floor floors[MAX_FLOORS];

// is this x (pixel) position within the gap <gap>?
bool is_in_gap(byte x, byte gap) {
  if (gap) {
    byte x1 = gap*16 + 4;
    return (x > x1 && x < x1+GAPSIZE*8-4);
  } else {
    return false;
  }
}

// is this ladder at (tile) position x within the gap?
bool ladder_in_gap(byte x, byte gap) {
  return gap && x >= gap && x < gap+GAPSIZE*2;
}

// create floors at start of game
void make_floors() {
  byte i;
  byte y = BOTTOM_FLOOR_Y;
  Floor* prevlev = &floors[0];
  for (i=0; i<MAX_FLOORS; i++) {
    Floor* lev = &floors[i];
    lev->height = rndint(2,4)*2;
    
      // only have gaps in higher floors
      lev->gap = i>1 ? rndint(2,8) : 4;
     
    if (i > 0) {
      lev->objtype = rndint(1,4);
      do {
        lev->objpos = rndint(1,14);
      } while (ladder_in_gap(lev->objpos, lev->gap));
    }
    lev->ypos = y;
    y += lev->height;
    prevlev = lev;
  }

}

// creete actors on floor_index, if slot is empty
void create_actors_on_floor(byte floor_index);

// draw a nametable line into the frame buffer at <row_height>
// 0 == bottom of stage
void draw_floor_line(byte row_height) {
  char buf[COLS];	// nametable buffer
  char attrs[8];	// attribute buffer 
  byte floor;		// floor counter
  byte dy;		// height in rows above floor
  byte rowy;		// row in nametable (0-59)
  word addr;		// nametable address
  byte i;		// loop counter
  // loop over all floors
  for (floor=0; floor<MAX_FLOORS; floor++) {
    Floor* lev = &floors[floor];
    // compute height in rows above floor
    dy = row_height - lev->ypos;
    // if below bottom floor (in basement)
    if (dy >= 255 - BOTTOM_FLOOR_Y) dy = 0;
    // does this floor intersect the desired row?
    if (dy < lev->height) {
      // first two rows (floor)?
      if (dy <= 1) {
        // iterate through all 32 columns
        for (i=0; i<COLS; i+=2) {
          if (dy) {
            buf[i] = CH_FLOOR;		// upper-left
            buf[i+1] = CH_FLOOR+2;	// upper-right
          } else {
            buf[i] = CH_FLOOR+1;	// lower-left
            buf[i+1] = CH_FLOOR+3;	// lower-right
          }
        }
        // is there a gap? if so, clear bytes
	if (lev->gap)
          memset(buf+lev->gap*2, 0, GAPSIZE);
      } else {
        // clear buffer
        memset(buf, 0, sizeof(buf));
        // draw walls
        if (floor < MAX_FLOORS-1) {
          buf[0] = CH_FLOOR+1;		// left side
          buf[COLS-1] = CH_FLOOR;	// right side
        }
        // draw ladders
       
      }
      // draw object, if it exists
      if (lev->objtype) {
        byte ch = lev->objtype*4 + CH_ITEM;
        if (dy == 2) {
          buf[lev->objpos*2] = ch+1;	// bottom-left
          buf[lev->objpos*2+1] = ch+3;	// bottom-right
        }
        else if (dy == 3) {
          buf[lev->objpos*2] = ch+0;	// top-left
          buf[lev->objpos*2+1] = ch+2;	// top-right
        }
      }
      break;
    }
  }
  // compute row in name buffer and address
  rowy = (ROWS-1) - (row_height % ROWS);
  addr = getntaddr(1, rowy);
  // copy attribute table (every 4th row)
  if ((addr & 0x60) == 0) {
    byte a;
    if (dy==1)
      a = 0x05;	// top of attribute block
    else if (dy==3)
      a = 0x50;	// bottom of attribute block
    else
      a = 0x00;	// does not intersect attr. block
    // write entire row of attribute blocks
    memset(attrs, a, 8);
    vrambuf_put(nt2attraddr(addr), attrs, 8);
  }
  // copy line to screen buffer
  vrambuf_put(addr, buf, COLS);
  // create actors on this floor, if needed
  if (dy == 0 && (floor >= 2)) {
    create_actors_on_floor(floor);
  }
}


// draw entire stage at current scroll position
// filling up entire name table
void draw_entire_stage() {
  byte y;
  for (y=2; y<ROWS; y++) {
    draw_floor_line(y);
    // allow buffer to flush, delaying a frame
    vrambuf_flush();
  }
}

// get Y pixel position for a given floor
word get_floor_yy(byte floor) {
  return floors[floor].ypos * 8 + 16;
}

// get Y ceiling position for a given floor
word get_ceiling_yy(byte floor) {
  return (floors[floor].ypos + floors[floor].height) * 8 + 16;
}


// set scrolling position
void set_scroll_pixel_yy(int yy) {
  // draw an offscreen line, every 8 pixels
  if ((yy & 7) == 0) {
    // scrolling upward or downward?
    if (yy > scroll_pixel_yy)
      draw_floor_line(scroll_tile_y + 30);	// above
    else
      draw_floor_line(scroll_tile_y - 30);	// below
  }
  // set scroll variables
  scroll_pixel_yy = yy;		// get scroll pos. in pixels
  scroll_tile_y = yy >> 3; 	// divide by 8
  // set scroll registers
  scroll(0, 479 - ((yy + 224) % 480));
}


// redraw a floor when object picked up
void refresh_floor(byte floor) {
  byte y = floors[floor].ypos;	// get floor bottom coordinate
  draw_floor_line(y+2);		// redraw 3rd line
  draw_floor_line(y+3);		// redraw 4th line
}

///// ACTORS

typedef enum ActorState {
  INACTIVE, STANDING, WALKING, CLIMBING, JUMPING, FALLING, PACING
};

typedef enum ActorType {
  ACTOR_PLAYER, ACTOR_ENEMY, ACTOR_RESCUE
};

typedef struct Actor {
  word yy;		// Y position in pixels (16 bit)
  byte x;		// X position in pixels (8 bit)
  byte floor;		// floor index
  byte state;		// ActorState
  sbyte yvel;		// Y velocity (when jumping)
  sbyte xvel;		// X velocity (when jumping)
  int name:2;		// ActorType (2 bits)
  int pal:2;		// palette color (2 bits)
  int dir:1;		// direction (0=right, 1=left)
  int onscreen:1;	// is actor onscreen?
} Actor;

Actor actors[MAX_ACTORS];	// all actors

// creete actors on floor_index, if slot is empty
void create_actors_on_floor(byte floor_index) {
  byte actor_index = (floor_index % (MAX_ACTORS-1)) + 1;
  struct Actor* a = &actors[actor_index];
  if (!a->onscreen) {
    Floor *floor = &floors[floor_index];
    a->state = STANDING;
    a->name = ACTOR_ENEMY;
    a->x = rand8();
    a->yy = get_floor_yy(floor_index);
    a->floor = floor_index;
    a->onscreen = 1;
    
  }
}

void draw_actor(byte i) {
  struct Actor* a = &actors[i];
  bool dir;
  const unsigned char* meta;
  byte x,y; // sprite variables
  // get screen Y position of actor
  int screen_y = SCREEN_Y_BOTTOM - a->yy + scroll_pixel_yy;
  // is it offscreen?
  if (screen_y > 192+8 || screen_y < -18) {
    a->onscreen = 0;
    return; // offscreen vertically
  }
  dir = a->dir;
  switch (a->state) {
    case INACTIVE:
      a->onscreen = 0;
      return; // inactive, offscreen
    case STANDING:
      meta = dir ? playerLStand : playerRStand;
      break;
    case WALKING:
      meta = playerRunSeq[((a->x >> 1) & 7) + (dir?0:8)];
      break;
    case JUMPING:
      meta = dir ? playerLJump : playerRJump;
      break;
    case FALLING:
      meta = dir ? playerLSad : playerRSad;
      break;
    case PACING:
      break;
  }
  // set sprite values, draw sprite
  x = a->x;
  y = screen_y;
  oam_meta_spr_pal(x, y, a->pal, meta);
  // is this actor 0? (player sprite)
  if (i == 0) {
    player_screen_y = y; // save last screen Y position
  }
  a->onscreen = 1; // if we drew the actor, consider it onscreen
  return;
}

// draw the scoreboard, right now just two digits
void draw_scoreboard() {
  oam_off = oam_spr(24+0, 24, '0'+(score >> 4), 2, oam_off);
  oam_off = oam_spr(24+8, 24, '0'+(score & 0xf), 2, oam_off);
}

void draw_lives() {

  oam_off = oam_spr(24+180, 24, '0'+(hit >> 4), 2, oam_off);
  oam_off = oam_spr(24+188, 24, '0'+(hit & 0xf), 2, oam_off);
}
// draw all sprites
void refresh_sprites() {
  byte i;
  // reset sprite index to 0
  oam_off = 0;
  // draw all actors
  for (i=0; i<MAX_ACTORS; i++)
    draw_actor(i);
  // draw scoreboard
  draw_scoreboard();
  draw_lives();
  // hide rest of actors
  oam_hide_rest(oam_off);
}


// should we scroll the screen upward?
void check_scroll_up() {
  if (player_screen_y < ACTOR_SCROLL_UP_Y) {
    set_scroll_pixel_yy(scroll_pixel_yy + 2);	// scroll up
  }
}

// should we scroll the screen downward?
void check_scroll_down() {
  if (player_screen_y > ACTOR_SCROLL_DOWN_Y && scroll_pixel_yy > 0) {
    set_scroll_pixel_yy(scroll_pixel_yy - 3);	// scroll down
  }
}

// actor falls down a floor
void fall_down(struct Actor* actor) {
  actor->floor--;
  actor->state = FALLING;
  actor->xvel = 0;
  actor->yvel = 0;
}

void title()
{
  ppu_off();
  pal_col(0,0x1B);	// set screen to dark blue
  pal_col(1,0x14);	// fuchsia
  pal_col(2,0x20);	// grey
  pal_col(3,0x30);
  
  vram_adr(NTADR_A(10, 9));
  vram_write("TOWER EXCAPE", 12);
  
  vram_adr(NTADR_A(6, 12));
  vram_write("Press ENTER to Start", 20);
  
  vram_adr(NTADR_A(9, 16));
  vram_write("Arrows to Move", 14);
  
  vram_adr(NTADR_A(9, 18));
  vram_write("SPACE to Jump", 13);
  
  ppu_on_all();
  //Start for 1 player
   while(1){     
     if(pad_trigger(0)&PAD_START)
     {
       return;

     }
    
    }
}

// move an actor (player or enemies)
// joystick - game controller mask
// scroll - if true, we should scroll screen (is player)
void move_actor(struct Actor* actor, byte joystick, bool scroll) {
  switch (actor->state) {
      
    case STANDING:
    case WALKING:
      // left/right has priority over climbing
      if (joystick & PAD_A) {
        actor->xvel = 0;
        actor->yvel = JUMP_VELOCITY;
	actor->state = JUMPING;


        if (joystick & PAD_LEFT) actor->xvel = -2;
        if (joystick & PAD_RIGHT) actor->xvel = 2;
        // play sound for player
        if (scroll) sfx_play(SND_JUMP,0);
        
      } else if (joystick & PAD_LEFT) {
        actor->x-=2;
        actor->dir = 3;
        actor->state = WALKING;
      } else if (joystick & PAD_RIGHT) {
        actor->x+=2;
        actor->dir = 0;
        actor->state = WALKING;
      }  else {
        actor->state = STANDING;
      }
      if (scroll) {
        check_scroll_up();
        check_scroll_down();
      }
      break;
      
    case FALLING:
      if (scroll) {
        check_scroll_up();
        check_scroll_down();
      }
    case JUMPING:
      actor->x += actor->xvel;
      actor->yy += actor->yvel/3;
      actor->yvel -= 1;
      
      //***if we jump at wall***
      if(actor->x == ACTOR_MIN_X)
      {
        actor->x = ACTOR_MAX_X;
        
      }
      else if(actor->x == ACTOR_MAX_X)
      {
        actor->x = ACTOR_MIN_X;
        
      }
      
      
      if (actor->yy <= get_floor_yy(actor->floor)) {
        if(actor->name == ACTOR_PLAYER && actor->yy >= floors[(actor->floor)+1].ypos * 3 + 12)
      {
         actor->floor++;
         
      }
	actor->yy = get_floor_yy(actor->floor);
        actor->state = STANDING;
      }
      
      break;
  }
  // don't allow player to travel past left/right edges of screen
 
  if (actor->x == ACTOR_MAX_X) actor->x = ACTOR_MIN_X; // we wrapped around right edge
  if (actor->x < ACTOR_MIN_X) actor->x = ACTOR_MAX_X;
  
  
  // if player lands in a gap, they fall (switch to JUMPING state)
  if (actor->name == ACTOR_PLAYER && actor->state <= WALKING && 
      is_in_gap(actor->x, floors[actor->floor].gap)) {
    fall_down(actor);
    actor->floor--;
  }
}

// should we pickup an object? only player does this
void pickup_object(Actor* actor) {
  Floor* floor = &floors[actor->floor];
  byte objtype = floor->objtype;
  // only pick up if there's an object, and if we're walking or standing
  if (objtype && actor->state <= WALKING) {
    byte objx = floor->objpos * 16;
    // is the actor close to the object?
    if (actor->x >= objx && actor->x < objx+16) {
      // clear the item from the floor and redraw
      floor->objtype = 0;
      refresh_floor(actor->floor);
      // did we hit a mine?
      if (objtype == ITEM_MINE) {
        // we hit a mine, fall down
        fall_down(actor);
        sfx_play(SND_HIT,0);
        vbright = 8; // flash
      }
      else if(objtype == ITEM_HEART && hit != 0){
        hit--;
      }
      else {
        // we picked up an object, add to score
        score = bcd_add(score, 3);
        sfx_play(SND_COIN,0);
      }
    }
  }
}

// read joystick 0 and move the player
void move_player() {
  byte joy = pad_poll(0);
  move_actor(&actors[0], joy, true);
  pickup_object(&actors[0]);
}

// returns absolute value of x
byte iabs(int x) {
  return x >= 0 ? x : -x;
}

// check to see if actor collides with any non-player actor
bool check_collision(Actor* a) {
  byte i;
  byte afloor = a->floor;
  // can't fall through basement
  if (afloor == 0) return false;
  // can't fall if already falling
  if (a->state == FALLING) return false;
  // iterate through entire list of actors
  for (i=1; i<MAX_ACTORS; i++) {
    Actor* b = &actors[i];
    // actors must be on same floor and within 8 pixels
    if (b->onscreen &&
        
        iabs(a->yy - b->yy) < 8 && 
        iabs(a->x - b->x) < 8) {
      score--;
      hit++;
      
      return true;
    }
  }
  return false;
}

///

const char* RESCUE_TEXT = 
  "You made it to the top";



// draw a message on the screen
void type_message(const char* charptr) {
  char ch;
  byte x,y;
  x = 4;
  // compute message y position relative to scroll
  y = ROWS*3 + 39 - scroll_tile_y;
  // repeat until end of string (0) is read
  while ((ch = *charptr++)) {
    while (y >= 60) y -= 60; // compute (y % 60)
    // newline character? go to start of next line
    if (ch == '\n') {
      x = 2;
      y++;
    } else {
      // put character into nametable
      vrambuf_put(getntaddr(x, y), &ch, 1);
      x++;
    }
    // typewriter sound
    sfx_play(SND_HIT,0);
    // flush buffer and wait a few frames
    vrambuf_flush();
    delay(5);
  }
}



// reward scene when player reaches roof
void rescue_scene() {
  music_stop();
  type_message(RESCUE_TEXT);
  type_message(RESCUE_TEXT);
  // wait 2 seconds
  delay(100);
}

void type_message2(const char* charptr) {
  char ch;
  byte x,y;
  x = 4;
  // compute message y position relative to scroll
  y = ROWS*3 + 39 - scroll_tile_y;
  // repeat until end of string (0) is read
  while ((ch = *charptr++)) {
    while (y >= 60) y -= 60; // compute (y % 60)
    // newline character? go to start of next line
    if (ch == '\n') {
      x = 2;
      y++;
    } else {
      // put character into nametable
      vrambuf_put(getntaddr(x, y+4), &ch, 1);
      x++;
    }
    // typewriter sound
    sfx_play(SND_HIT,0);
    // flush buffer and wait a few frames
    vrambuf_flush();
    delay(5);
  }
}
const char* GAMEOVER_TEXT = 
  "You fell down the tower!";

const char* RESTART_TEXT = 
  "Press ENTER to play again!";

void game_over() {
  music_stop();
  type_message(GAMEOVER_TEXT);
  type_message2(RESTART_TEXT);

  // wait 2 seconds
  //delay(100);
  while(1){     
     if(pad_trigger(0)&PAD_START)
     {
       return;
     }
    
    }
}


// game loop
void play_scene() {
  byte i;
  // initialize actors array  
  memset(actors, 0, sizeof(actors));
  actors[0].state = STANDING;
  actors[0].name = ACTOR_PLAYER;
  actors[0].pal = 3;
  actors[0].x = 400;
  actors[0].floor = 0;
  actors[0].yy = get_floor_yy(0);
  // put actor at bottom
  set_scroll_pixel_yy(0);
  // draw initial view of level into nametable
  draw_entire_stage();
  // repeat until player reaches the roof
  while (actors[0].floor != MAX_FLOORS-1) {
    // flush VRAM buffer (waits next frame)
    vrambuf_flush();
    refresh_sprites();
    move_player();
    // move all the actors
    for (i=1; i<MAX_ACTORS; i++) {
      move_actor(&actors[i], rand8(), false);
    }
    // see if the player hit another actor
    if (check_collision(&actors[0])) {
      fall_down(&actors[0]);
      actors[0].floor--;
      sfx_play(SND_HIT,0);
      vbright = 8; // flash
    }
    if(hit == 3)
    {
      music_stop();
      game_over();

      //title();
      return;
    }
    // flash effect
    if (vbright > 4) {
      pal_bright(--vbright);
    }
  }
  // player reached goal; reward scene  
  rescue_scene();
}

/*{pal:"nes",layout:"nes"}*/
const char PALETTE[32] = { 
  0x22,			// background color

  0x16,0x16,0x20, 0x0,	// ladders and pickups
  0x36,0x20,0x0D, 0x0,	// floor blocks
  0x36,0x20,0x0D, 0x0,
  0x36,0x20,0x0D, 0x0,

  0x16,0x37,0x0D, 0x0,	// enemy sprites
  0x00,0x37,0x25, 0x0,	// rescue person
  0x0d,0x2d,0x3a, 0x0,
  0x0D,0x1B,0x1B	// player sprites
};

// set up PPU
void setup_graphics() {
  ppu_off();
  oam_clear();
  pal_all(PALETTE);
  vram_adr(0x2000);
  vram_fill(CH_BLANK, 0x1000);
  vrambuf_clear();
  set_vram_update(updbuf);
  ppu_on_all();
}

// set up famitone library
void setup_sounds() {
  famitone_init(danger_streets_music_data);
  sfx_init(demo_sounds);
  nmi_set_callback(famitone_update);
}

// main program
void main() {
  setup_sounds();
  title();

  
  while (1) {
    setup_graphics();		// setup PPU, clear screen
    sfx_play(SND_START,0);	// play starting sound
    make_floors();		// make random level
    music_play(0);		// start the music
    play_scene();
    hit = 0;
    score = 0;
    //return;
// play the level
  }
}
