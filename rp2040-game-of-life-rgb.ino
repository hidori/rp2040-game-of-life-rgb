// Game of Life RGB

#include <Adafruit_NeoPixel.h>

// LED matrix (levels are 0-255 per channel, white lights each channel at LED_LEVEL / LED_WHITE_DIVISOR)
#define LED_PIN 14
#define LED_COUNT 64
#define LED_LEVEL 32
#define LED_WHITE_DIVISOR 2

// Board and initial cells
#define BOARD_WIDTH 8
#define BOARD_HEIGHT 8
#define CELL_COUNT (BOARD_WIDTH * BOARD_HEIGHT)
#define INITIAL_CELL_PERCENT 40
#define INITIAL_CELL_PERCENT_RANGE 10

// Cell values
#define CELL_DEAD 0
#define CELL_RED 1
#define CELL_GREEN 2
#define CELL_BLUE 3
#define COLOR_COUNT 3

// Cell packing (2 bits per cell)
#define CELL_BITS 2
#define CELLS_PER_BYTE (8 / CELL_BITS)
#define CELL_MASK ((1 << CELL_BITS) - 1)
#define CELL_BYTES (CELL_COUNT / CELLS_PER_BYTE)

// Pixel values (CELL_DEAD to CELL_BLUE are shared with the cells)
#define PIXEL_WHITE 4

// Offscreen buffer packing (4 bits per LED)
#define PIXEL_BITS 4
#define PIXELS_PER_BYTE (8 / PIXEL_BITS)
#define PIXEL_MASK ((1 << PIXEL_BITS) - 1)
#define PIXEL_BYTES (LED_COUNT / PIXELS_PER_BYTE)

// Color rules (predation and mutation)
#define PREY_THRESHOLD 2
#define PREY_SURVIVAL_PERCENT 20
#define MUTATION_PERCENT 20

// Time-division lighting
#define GROUP_SIZE 8
#define GROUP_COUNT (LED_COUNT / GROUP_SIZE)

// Demo (duration in ms)
#define DEMO_BLINK_COUNT 3
#define DEMO_BLINK_DURATION 100

// Game timing (interval and duration in ms)
#define UPDATE_INTERVAL 1000
#define TRANSITION_DURATION 100
#define EQUILIBRIUM_GENERATIONS 10
#define EXTINCTION_GENERATIONS 3

// USB serial connection wait (ms)
#define SERIAL_WAIT_TIMEOUT 3000

// LED matrix driver
Adafruit_NeoPixel leds(LED_COUNT, LED_PIN, NEO_GRB + NEO_KHZ800);

// Offscreen buffer (physical LED order, packed pixel values)
// Only core 0 writes it, and core 1 only reads it
uint8_t offscreen[PIXEL_BYTES];

// Set by setup1() when its initialization is done
volatile bool setup1Done = false;

// Game of Life cells (logical coordinates, packed CELL_DEAD or a color)
uint8_t cells[CELL_BYTES];

// Next generation being computed by updateCells()
uint8_t nextCells[CELL_BYTES];

// Previous generation (for the transition)
uint8_t prevCells[CELL_BYTES];

// millis() at the last update
unsigned long lastUpdateMillis;

// Generation number in the current game
unsigned long generation = 0;

// Count of each color in the last generation (for the equilibrium detection)
int lastColorCounts[COLOR_COUNT + 1];

// Number of generations in a row with unchanged color counts
int unchangedGenerations = 0;

// Most frequent color at the end of the last game, used by demo() (CELL_DEAD: white)
uint8_t demoColor = CELL_DEAD;

// ---- Entry points ----

// Core 0: initializes the game side and waits for setup1()
void setup() {
  Serial.begin(115200);
  waitSerial();
  Serial.printf("setup: start\n");

  randomSeed(rp2040.hwrand32());

  while (!setup1Done) {
  }
  Serial.printf("setup: setup1 done\n");
}

// Core 0: shows the demo, then runs a game until the cells reach an equilibrium
void loop() {
  demo();
  game();
}

// Core 1: initializes the LED matrix and the offscreen buffer, then notifies setup()
void setup1() {
  waitSerial();
  Serial.printf("setup1: start\n");

  leds.begin();
  leds.clear();
  leds.show();
  Serial.printf("setup1: leds initialized\n");

  memset(offscreen, 0, sizeof(offscreen));
  Serial.printf("setup1: offscreen cleared\n");

  setup1Done = true;
  Serial.printf("setup1: done\n");
}

// Core 1: shows one group of the offscreen buffer (time-division lighting)
void loop1() {
  static size_t groupIndex = 0;

  // Group: GROUP_SIZE cells sampled from the logical origin, column first then row
  leds.clear();
  for (size_t i = 0; i < GROUP_SIZE; i++) {
    size_t cellIndex = groupIndex * GROUP_SIZE + i;
    size_t ledIndex = ledIndexOf(cellIndex % BOARD_WIDTH, cellIndex / BOARD_WIDTH);
    leds.setPixelColor(ledIndex, ledColorOf(getPixel(ledIndex), LED_LEVEL));
  }
  leds.show();

  groupIndex = (groupIndex + 1) % GROUP_COUNT;
}

// ---- Game (core 0) ----

// Blinks all LEDs DEMO_BLINK_COUNT times in the demo color
void demo() {
  uint8_t pixel = (demoColor != CELL_DEAD) ? demoColor : PIXEL_WHITE;

  for (int i = 0; i < DEMO_BLINK_COUNT; i++) {
    fillPixels(pixel);
    delay(DEMO_BLINK_DURATION);

    memset(offscreen, 0, sizeof(offscreen));
    delay(DEMO_BLINK_DURATION);
  }
}

// Runs a game from a new initial state until the cells reach an equilibrium
void game() {
  long count = initCells();
  Serial.printf("game: initial cells %ld\n", count);

  lastUpdateMillis = millis() - UPDATE_INTERVAL;
  generation = 0;
  for (uint8_t color = CELL_RED; color <= CELL_BLUE; color++) {
    lastColorCounts[color] = -1;
  }
  unchangedGenerations = 0;

  while (1) {
    unsigned long now = millis();
    if (now - lastUpdateMillis < UPDATE_INTERVAL) {
      continue;
    }
    lastUpdateMillis = now;

    // generation > 0: the cells have been updated in this game
    if (generation > 0) {
      reflectTransition();
      delay(TRANSITION_DURATION);
    }

    int counts[COLOR_COUNT + 1];
    reflectCells(counts);
    Serial.printf("gen %lu: R %d G %d B %d\n", generation, counts[CELL_RED], counts[CELL_GREEN], counts[CELL_BLUE]);

    if (isEquilibrium(counts)) {
      demoColor = modeColor(counts);
      Serial.printf("game: equilibrium, demo color %d\n", demoColor);
      return;
    }

    updateCells();
    generation++;
  }
}

// Equilibrium (including extinction): the count of each color has not changed for EQUILIBRIUM_GENERATIONS generations
// (EXTINCTION_GENERATIONS when extinct)
bool isEquilibrium(const int counts[]) {
  bool unchanged = true;
  for (uint8_t color = CELL_RED; color <= CELL_BLUE; color++) {
    if (counts[color] != lastColorCounts[color]) {
      unchanged = false;
    }
    lastColorCounts[color] = counts[color];
  }

  if (unchanged) {
    unchangedGenerations++;
  } else {
    unchangedGenerations = 0;
  }

  bool extinct = (counts[CELL_RED] + counts[CELL_GREEN] + counts[CELL_BLUE]) == 0;
  return unchangedGenerations >= (extinct ? EXTINCTION_GENERATIONS : EQUILIBRIUM_GENERATIONS);
}

// ---- Cells (Game of Life rules) ----

// Places the initial cells at random and returns the number of cells
long initCells() {
  memset(cells, 0, sizeof(cells));

  long countMin = (CELL_COUNT * (INITIAL_CELL_PERCENT - INITIAL_CELL_PERCENT_RANGE) + 99) / 100;
  long countMax = (CELL_COUNT * (INITIAL_CELL_PERCENT + INITIAL_CELL_PERCENT_RANGE)) / 100;
  long count = random(countMin, countMax + 1);

  for (long i = 0; i < count;) {
    size_t x = random(BOARD_WIDTH);
    size_t y = random(BOARD_HEIGHT);
    if (getCell(cells, x, y) != CELL_DEAD) {
      continue;
    }
    setCell(cells, x, y, randomColor());
    i++;
  }

  return count;
}

// R, G or B with equal probability
uint8_t randomColor() {
  return CELL_RED + random(COLOR_COUNT);
}

// Color that eats the color (R eats G, G eats B, B eats R)
uint8_t predatorOf(uint8_t color) {
  switch (color) {
  case CELL_RED:
    return CELL_BLUE;
  case CELL_GREEN:
    return CELL_RED;
  default:
    return CELL_GREEN;
  }
}

// Counts the neighbors of each color into counts and returns the number of alive neighbors
int countNeighbors(size_t x, size_t y, int counts[]) {
  memset(counts, 0, sizeof(int) * (COLOR_COUNT + 1));

  for (int dy = -1; dy <= 1; dy++) {
    for (int dx = -1; dx <= 1; dx++) {
      if (dx == 0 && dy == 0) {
        continue;
      }
      size_t nx = (x + BOARD_WIDTH + dx) % BOARD_WIDTH;
      size_t ny = (y + BOARD_HEIGHT + dy) % BOARD_HEIGHT;
      counts[getCell(cells, nx, ny)]++;
    }
  }

  return counts[CELL_RED] + counts[CELL_GREEN] + counts[CELL_BLUE];
}

// Most frequent color in counts (CELL_DEAD if not unique or no cells)
uint8_t modeColor(const int counts[]) {
  uint8_t mode = CELL_RED;
  bool unique = true;
  for (uint8_t color = CELL_GREEN; color <= CELL_BLUE; color++) {
    if (counts[color] > counts[mode]) {
      mode = color;
      unique = true;
    } else if (counts[color] == counts[mode]) {
      unique = false;
    }
  }

  return unique ? mode : CELL_DEAD;
}

// Newborn color: the most frequent neighbor color (random if not unique), or a random color by mutation
uint8_t birthColor(const int counts[]) {
  if (random(100) < MUTATION_PERCENT) {
    return randomColor();
  }

  uint8_t mode = modeColor(counts);
  return (mode != CELL_DEAD) ? mode : randomColor();
}

// Updates the cells to the next generation (B3/S23 with predation and mutation)
void updateCells() {
  int counts[COLOR_COUNT + 1];

  for (size_t y = 0; y < BOARD_HEIGHT; y++) {
    for (size_t x = 0; x < BOARD_WIDTH; x++) {
      int neighbors = countNeighbors(x, y, counts);
      uint8_t cell = getCell(cells, x, y);
      uint8_t next = CELL_DEAD;

      if (cell != CELL_DEAD) {
        bool survive = (neighbors == 2 || neighbors == 3);
        // Prey: the survival probability is PREY_SURVIVAL_PERCENT of the normal one
        if (survive && counts[predatorOf(cell)] >= PREY_THRESHOLD && random(100) >= PREY_SURVIVAL_PERCENT) {
          survive = false;
        }
        next = survive ? cell : CELL_DEAD;
      } else if (neighbors == 3) {
        next = birthColor(counts);
      }

      setCell(nextCells, x, y, next);
    }
  }

  memcpy(prevCells, cells, sizeof(cells));
  memcpy(cells, nextCells, sizeof(cells));
}

// Cell value at the logical coordinates in the packed cell buffer
uint8_t getCell(const uint8_t buffer[], size_t x, size_t y) {
  size_t index = y * BOARD_WIDTH + x;
  uint8_t shift = (index % CELLS_PER_BYTE) * CELL_BITS;
  return (buffer[index / CELLS_PER_BYTE] >> shift) & CELL_MASK;
}

// Sets the cell value at the logical coordinates in the packed cell buffer
void setCell(uint8_t buffer[], size_t x, size_t y, uint8_t cell) {
  size_t index = y * BOARD_WIDTH + x;
  uint8_t shift = (index % CELLS_PER_BYTE) * CELL_BITS;
  uint8_t *b = &buffer[index / CELLS_PER_BYTE];
  *b = (*b & ~(CELL_MASK << shift)) | ((cell & CELL_MASK) << shift);
}

// ---- Display (cells -> offscreen buffer) ----

// Pixel value of the physical LED in the offscreen buffer
uint8_t getPixel(size_t ledIndex) {
  uint8_t shift = (ledIndex % PIXELS_PER_BYTE) * PIXEL_BITS;
  return (offscreen[ledIndex / PIXELS_PER_BYTE] >> shift) & PIXEL_MASK;
}

// Sets the pixel value of the physical LED in the offscreen buffer
// (a byte is written at once, so core 1 reads either the old or the new byte)
void setPixel(size_t ledIndex, uint8_t pixel) {
  uint8_t shift = (ledIndex % PIXELS_PER_BYTE) * PIXEL_BITS;
  uint8_t *b = &offscreen[ledIndex / PIXELS_PER_BYTE];
  *b = (*b & ~(PIXEL_MASK << shift)) | ((pixel & PIXEL_MASK) << shift);
}

// Sets all pixels of the offscreen buffer to the pixel value
void fillPixels(uint8_t pixel) {
  uint8_t b = 0;
  for (uint8_t i = 0; i < PIXELS_PER_BYTE; i++) {
    b |= (pixel & PIXEL_MASK) << (i * PIXEL_BITS);
  }
  memset(offscreen, b, sizeof(offscreen));
}

// Logical coordinates -> physical LED index
size_t ledIndexOf(size_t x, size_t y) {
  return (BOARD_HEIGHT - 1 - y) * BOARD_WIDTH + (BOARD_WIDTH - 1 - x);
}

// LED color of the pixel value at the level (0 for CELL_DEAD, each channel at level / LED_WHITE_DIVISOR for white)
uint32_t ledColorOf(uint8_t pixel, uint8_t level) {
  switch (pixel) {
  case CELL_RED:
    return leds.Color(level, 0, 0);
  case CELL_GREEN:
    return leds.Color(0, level, 0);
  case CELL_BLUE:
    return leds.Color(0, 0, level);
  case PIXEL_WHITE:
    return leds.Color(level / LED_WHITE_DIVISOR, level / LED_WHITE_DIVISOR, level / LED_WHITE_DIVISOR);
  default:
    return 0;
  }
}

// Reflects the cells to the offscreen buffer and returns the number of cells of each color in counts
void reflectCells(int counts[]) {
  memset(counts, 0, sizeof(int) * (COLOR_COUNT + 1));

  for (size_t y = 0; y < BOARD_HEIGHT; y++) {
    for (size_t x = 0; x < BOARD_WIDTH; x++) {
      uint8_t cell = getCell(cells, x, y);
      setPixel(ledIndexOf(x, y), cell);
      counts[cell]++;
    }
  }
}

// Reflects the last update to the offscreen buffer: born or dead cells white, survivors as usual
// (a survivor never changes its color, so a changed cell is a born or dead one)
void reflectTransition() {
  for (size_t y = 0; y < BOARD_HEIGHT; y++) {
    for (size_t x = 0; x < BOARD_WIDTH; x++) {
      uint8_t cell = getCell(cells, x, y);
      setPixel(ledIndexOf(x, y), (getCell(prevCells, x, y) != cell) ? PIXEL_WHITE : cell);
    }
  }
}

// ---- Serial ----

// Wait for the USB serial connection so that startup logs are not lost
void waitSerial() {
  unsigned long start = millis();
  while (!Serial && millis() - start < SERIAL_WAIT_TIMEOUT) {
  }
}
