/*
Build:
	gcc nmea_0183_analysis.c -o nmea_0183_analysis -D_GNU_SOURCE -D__USE_XOPEN

*/
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <signal.h>
#include <fcntl.h>
#include <time.h>
#include <termios.h>

/************************* NMEA-0183共通 *****************************/
#define NMEA_MAXSAT (32)	/*  */

typedef struct _nmeaTIME
{
    int     year;       /* Years since 1900 */
    int     mon;        /* Months since January - [0,11] */
    int     day;        /* Day of the month - [1,31] */
    int     hour;       /* Hours since midnight - [0,23] */
    int     min;        /* Minutes after the hour - [0,59] */
    int     sec;        /* Seconds after the minute - [0,59] */
    int     hsec;       /* Hundredth part of second - [0,99] */
} nmeaTIME;

typedef struct _nmeaSATELLITE
{
    int     id;         /* Satellite PRN number */
    int     in_use;     /* Used in position fix */
    int     elv;        /* Elevation in degrees, 90 maximum */
    int     azimuth;    /* Azimuth, degrees from true north, 000 to 359 */
    int     sig;        /* Signal, 00-99 dB */
} nmeaSATELLITE;

typedef struct _nmeaSATINFO
{
    int     added;      /*  */
    int     inview;     /*  */
    nmeaSATELLITE sat[NMEA_MAXSAT]; /*  */
} nmeaSATINFO;


typedef struct _nmeaINFO
{
    int     smask;      /*  */
    nmeaTIME utc;       /*  */
    int     sig;        /*  */
    int     fix;        /*  */
    double  PDOP;       /*  */
    double  HDOP;       /*  */
    double  VDOP;       /*  */
    double  lat;        /*  */
    double  lon;        /*  */
    double  elv;        /*  */
    double  speed;      /*  */
    double  direction;  /*  */
    double  declination;/*  */
    nmeaSATINFO satinfo;/*  */
} nmeaINFO;

nmeaINFO g_info;


#define NMEA_MAX_LENGTH		82	/* NMEA sentence max length, including \r\n (chars) */
#define NMEA_END_CHAR_1		'\r'	/* NMEA sentence endings, should be \r\n according the NMEA 0183 standard */
#define NMEA_END_CHAR_2		'\n'
#define NMEA_PREFIX_LENGTH	5	/* NMEA sentence prefix length (num chars), Ex: GPGLL */

#define NMEA_PARSER_PREFIX(parser, type_prefix) strncpy(parser->type_word, type_prefix, NMEA_PREFIX_LENGTH)
#define NMEA_PARSER_TYPE(parser, nmea_type)     parser->type = nmea_type

#define NMEA_TIME_FORMAT	"%H%M%S"
#define NMEA_TIME_FORMAT_LEN	6

#define NMEA_DATE_FORMAT	"%d%m%y"
#define NMEA_DATE_FORMAT_LEN	6

#define ARRAY_LENGTH(a) (sizeof a / sizeof (a[0]))

typedef unsigned char uint8_t;

/* NMEA sentence types */
typedef enum {
	NMEA_UNKNOWN,
	NMEA_GPGGA,
	NMEA_GPGLL,
	NMEA_GPRMC,
	NMEA_GPGSV
} nmea_t;

/**
 * NMEA data base struct
 * This struct will be extended by the parser data structs (ex: nmea_gpgll_s).
 */
typedef struct {
	nmea_t type;
	int errors;
} nmea_s;

/* NMEA cardinal direction types */
typedef char nmea_cardinal_t;
#define NMEA_CARDINAL_DIR_NORTH		(nmea_cardinal_t) 'N'
#define NMEA_CARDINAL_DIR_EAST		(nmea_cardinal_t) 'E'
#define NMEA_CARDINAL_DIR_SOUTH		(nmea_cardinal_t) 'S'
#define NMEA_CARDINAL_DIR_WEST		(nmea_cardinal_t) 'W'
#define NMEA_CARDINAL_DIR_UNKNOWN	(nmea_cardinal_t) '\0'

/* GPS position struct */
typedef struct {
	double minutes;
	int degrees;
	nmea_cardinal_t cardinal;
} nmea_position;

typedef struct {
	nmea_t type;
	char type_word[5];
	nmea_s *data;
} nmea_parser_s;

typedef int (*allocate_data_f) (nmea_parser_s *);
typedef int (*set_default_f) (nmea_parser_s *);
typedef int (*free_data_f) (nmea_s *);
typedef int (*parse_f) (nmea_parser_s *, char *, int);
typedef int (*init_f) (nmea_parser_s *);

typedef struct {
	nmea_parser_s parser;
	int errors;
	void *handle;

	/* Functions */
	allocate_data_f allocate_data;
	set_default_f set_default;
	free_data_f free_data;
	parse_f parse;
} nmea_parser_module_s;

int parsers_no;
nmea_parser_module_s **g_parsers;
char *support_sentence[] = {"GSV", "RMC"};

char buffer[4096];
int gps_fd;

/************************* "GPRMC" *****************************/
typedef struct {
	nmea_s base;
	nmea_position longitude;
	nmea_position latitude;
	double speed;
	struct tm time;
} nmea_gprmc_s;

/* Value indexes */
#define NMEA_GPRMC_LATITUDE		2
#define NMEA_GPRMC_LATITUDE_CARDINAL	3
#define NMEA_GPRMC_LONGITUDE		4
#define NMEA_GPRMC_LONGITUDE_CARDINAL	5
#define NMEA_GPRMC_TIME			0
#define NMEA_GPRMC_DATE			8
#define NMEA_GPRMC_SPEED		6

/************************* "GPGSV" *****************************/
typedef struct {
	unsigned int sentences;		/* Number of sentences for full data */
	unsigned int sentence_number;	/* Current sentence number */
	unsigned int satellites;	/* Number of satellites in view */
	unsigned int prn;		/* Satellite PRN number */
	int elevation;			/* Elevation, degrees */
	int azimuth;			/* Azimuth, degrees */
	int snr;			/* SNR - higher is better */
} nmea_gpgsv_s;

enum {
	NMEA_GPGSV_SENTENCES,		/* index 0 Sentence数 */
	NMEA_GPGSV_SENTENCE_NUMBER,	/* index 1 Sentence番号 */
	NMEA_GPGSV_SATELLITES,		/* index 2 可視衛星数 */
	
	NMEA_GPGSV_PRN1,		/* index 3 衛星ID */
	NMEA_GPGSV_ELEVATION1,		/* index 4 仰角 */
	NMEA_GPGSV_AZIMUTH1,		/* index 5 方位角 */
	NMEA_GPGSV_SNR1,		/* index 6 信号強度(C/N) */
	
	NMEA_GPGSV_PRN2,		/* index 7 衛星ID */
	NMEA_GPGSV_ELEVATION2,		/* index 8 仰角 */
	NMEA_GPGSV_AZIMUTH2,		/* index 9 方位角 */
	NMEA_GPGSV_SNR2,		/* index 10 信号強度(C/N) */
	
	NMEA_GPGSV_PRN3,		/* index 11 衛星ID */
	NMEA_GPGSV_ELEVATION3,		/* index 12 仰角 */
	NMEA_GPGSV_AZIMUTH3,		/* index 13 方位角 */
	NMEA_GPGSV_SNR3,		/* index 14 信号強度(C/N) */
	
	NMEA_GPGSV_PRN4,		/* index 15 衛星ID */
	NMEA_GPGSV_ELEVATION4,		/* index 16 仰角 */
	NMEA_GPGSV_AZIMUTH4,		/* index 17 方位角 */
	NMEA_GPGSV_SNR4,		/* index 18 信号強度(C/N) */
};
/*************************************************************/


/************************* NMEA-0183共通 *****************************/
uint8_t nmea_get_checksum(const char *sentence)
{
	const char *n = sentence + 1;
	uint8_t chk = 0;

	/* While current char isn't '*' or sentence ending (newline) */
	while ('*' != *n && NMEA_END_CHAR_1 != *n && '\0' != *n) {
		chk ^= (uint8_t) *n;
		n++;
	}

	return chk;
}

int nmea_has_checksum(const char *sentence, size_t length)
{
	if ('*' == sentence[length - 5]) {
		return 0;
	}

	return -1;
}

int nmea_validate(const char *sentence, size_t length, int check_checksum)
{
	const char *n;

	/* should have atleast 9 characters */
	if (9 > length) {
		return -1;
	}

	/* should be less or equal to 82 characters */
	if (NMEA_MAX_LENGTH < length) {
		return -1;
	}

	/* should start with $ */
	if ('$' != *sentence) {
		return -1;
	}

	/* should end with \r\n, or other... */
	if (NMEA_END_CHAR_2 != sentence[length - 1] || NMEA_END_CHAR_1 != sentence[length - 2]) {
		return -1;
	}

	/* should have a 5 letter, uppercase word */
	n = sentence;
	while (++n < sentence + 6) {
		if (*n < 'A' || *n > 'Z') {
			/* not uppercase letter */
			return -1;
		}
	}

	/* should have a comma after the type word */
	if (',' != sentence[6]) {
		return -1;
	}

	/* check for checksum */
	if (1 == check_checksum && 0 == nmea_has_checksum(sentence, length)) {
		uint8_t actual_chk;
		uint8_t expected_chk;
		char checksum[3];

		checksum[0] = sentence[length - 4];
		checksum[1] = sentence[length - 3];
		checksum[2] = '\0';
		actual_chk = nmea_get_checksum(sentence);
		expected_chk = (uint8_t) strtol(checksum, NULL, 16);
		if (expected_chk != actual_chk) {
			return -1;
		}
	}

	return 0;
}

nmea_parser_module_s* nmea_get_parser_by_sentence(const char *sentence)
{
	int i;
	nmea_parser_module_s *parser;

	for (i = 0; i < parsers_no; i++) {
		parser = g_parsers[i];
		if (0 == strncmp(sentence + 1, parser->parser.type_word, NMEA_PREFIX_LENGTH)) {
			return parser;
		}
	}

	return (nmea_parser_module_s *) NULL;
}

nmea_t nmea_get_type(const char *sentence)
{
	nmea_parser_module_s *parser = nmea_get_parser_by_sentence(sentence);
	if (NULL == parser) {
		return NMEA_UNKNOWN;
	}

	return parser->parser.type;
}

static char* _crop_sentence(char *sentence, size_t length)
{
	/* Skip type word, 7 characters (including $ and ,) */
	sentence += NMEA_PREFIX_LENGTH + 2;

	/* Null terminate before end of line/sentence, 2 characters */
	sentence[length - 9] = '\0';

	/* Remove checksum, if there is one */
	if ('*' == sentence[length - 12]) {
		sentence[length - 12] = '\0';
	}

	return sentence;
}

static int _split_string_by_comma(char *string, char **values, int max_values)
{
	int i = 0;

	values[i++] = string;
	while (i < max_values && NULL != (string = strchr(string, ','))) {
		*string = '\0';
		values[i++] = ++string;
	}

	return i;
}

nmea_parser_module_s* nmea_get_parser_by_type(nmea_t type)
{
	int i;

	for (i = 0; i < parsers_no; i++) {
		if (type == g_parsers[i]->parser.type) {
			return g_parsers[i];
		}
	}

	return (nmea_parser_module_s *) NULL;
}

static int _is_value_set(const char *value)
{
	if (NULL == value || '\0' == *value) {
		return -1;
	}

	return 0;
}

int nmea_position_parse(char *s, nmea_position *pos)
{
	char *cursor;

	pos->degrees = 0;
	pos->minutes = 0;

	if (s == NULL || *s == '\0') {
		return -1;
	}

	/* decimal minutes */
	if (NULL == (cursor = strchr(s, '.'))) {
		return -1;
	}

	/* minutes starts 2 digits before dot */
	cursor -= 2;
	pos->minutes = atof(cursor);
	*cursor = '\0';

	/* integer degrees */
	cursor = s;
	pos->degrees = atoi(cursor);

	return 0;
}

nmea_cardinal_t nmea_cardinal_direction_parse(char *s)
{
	if (NULL == s || '\0'== *s) {
		return NMEA_CARDINAL_DIR_UNKNOWN;
	}

	switch (*s) {
	case NMEA_CARDINAL_DIR_NORTH:
		return NMEA_CARDINAL_DIR_NORTH;
	case NMEA_CARDINAL_DIR_EAST:
		return NMEA_CARDINAL_DIR_EAST;
	case NMEA_CARDINAL_DIR_SOUTH:
		return NMEA_CARDINAL_DIR_SOUTH;
	case NMEA_CARDINAL_DIR_WEST:
		return NMEA_CARDINAL_DIR_WEST;
	default:
		break;
	}

	return NMEA_CARDINAL_DIR_UNKNOWN;
}

int nmea_time_parse(char *s, struct tm *time)
{
	char *rv;

	memset(time, 0, sizeof (struct tm));

	if (s == NULL || *s == '\0') {
		return -1;
	}

	rv = strptime(s, NMEA_TIME_FORMAT, time);
	if (NULL == rv || (int) (rv - s) != NMEA_TIME_FORMAT_LEN) {
		return -1;
	}

	return 0;
}

int nmea_date_parse(char *s, struct tm *time)
{
	char *rv;

	// Assume it has been already cleared
	// memset(time, 0, sizeof (struct tm));

	if (s == NULL || *s == '\0') {
		return -1;
	}

	rv = strptime(s, NMEA_DATE_FORMAT, time);
	if (NULL == rv || (int) (rv - s) != NMEA_DATE_FORMAT_LEN) {
		return -1;
	}

	return 0;
}


nmea_s* nmea_0183_parse(char *sentence, size_t length, int check_checksum)
{
	unsigned int n_vals, val_index;
	char *value, *val_string;
	char *values[255];
	nmea_parser_module_s *parser;
	nmea_t type;
	int i;

	/* Validate sentence string */
	if (-1 == nmea_validate(sentence, length, check_checksum)) {
		return (nmea_s *) NULL;
	}

	type = nmea_get_type(sentence);
	if (NMEA_UNKNOWN == type) {
		//printf("nmea type: NMEA_UNKNOWN\n");
		return (nmea_s *) NULL;
	}

	/* debug */
	if (type == NMEA_GPGSV) 
		printf("%s", sentence);

	/* Crop sentence from type word and checksum */
	val_string = _crop_sentence(sentence, length);
	if (NULL == val_string) {
	      	return (nmea_s *) NULL;
	}

	/* Split the sentence into values */
	n_vals = _split_string_by_comma(val_string, values, ARRAY_LENGTH(values));
	if (0 == n_vals) {
		return (nmea_s *) NULL;
	}

	/* Get the right parser */
	parser = nmea_get_parser_by_type(type);
	if (NULL == parser) {
		return (nmea_s *) NULL;
	}

	/* Allocate memory for parsed data */
	parser->allocate_data((nmea_parser_s *) parser);
	if (NULL == parser->parser.data) {
		return (nmea_s *) NULL;
	}

	/* Set default values */
	parser->set_default((nmea_parser_s *) parser);
	parser->errors = 0;

	/* Loop through the values and parse them... */
	for (val_index = 0; val_index < n_vals; val_index++) {
		value = values[val_index];
		if (-1 == _is_value_set(value)) {
			continue;
		}

		if (-1 == parser->parse((nmea_parser_s *) parser, value, val_index)) {
			parser->errors++;
		}
	}

	parser->parser.data->type = type;
	parser->parser.data->errors = parser->errors;

	return parser->parser.data;
}

void nmea_free(nmea_s *data)
{
	nmea_parser_module_s *parser;

	if (NULL == data) {
		return;
	}

	parser = nmea_get_parser_by_type(data->type);
	if (NULL == parser) {
		return;
	}

	parser->free_data(data);
}
/***************************************************************************************/

void sig_quit(int signum)
{
	close(gps_fd);
	exit(EXIT_SUCCESS);
}

int register_signals()
{
	struct sigaction kill_action;
	kill_action.sa_handler = sig_quit;

	if (-1 == sigemptyset(&kill_action.sa_mask)) {
		perror("sigemptyset");
		return -1;
	}
	kill_action.sa_flags = 0;
	if (-1 == sigaction(SIGINT, &kill_action, NULL)) {
		perror("sigaction");
		return -1;
	}

	return 0;
}

int send_command(char *cmd)
{
	int ret = 0;
	int resp_bytes = 0;
	char resp_buffer[20] = {0};
	char *ptr = NULL;

	printf("send command: %s", cmd);
	if (write(gps_fd, cmd, strlen(cmd)) < 0) {
		perror("write /dev/ttymxc2 error, exit!\n");
		return -1;
	} else {
retry:
		/* check response */
		resp_bytes = read(gps_fd, resp_buffer, sizeof(resp_buffer));
		if (-1 == resp_bytes) {
			perror("can't read response, retry!\n");
			goto retry;
		} else {
			//printf("read %d bytes response data:%s", resp_bytes, resp_buffer);
			ptr = strstr(resp_buffer, "Done");
			if ( ptr == NULL) {
				goto retry;
			} else {
				printf("send command success!\n\n");
				ret = 0;
			}
		}
	}
	return ret;
}

int init_host_serial_port(void)
{
	struct termios tio;
	
	memset(&tio, 0, sizeof (tio));

	if (tcgetattr(gps_fd, &tio) != 0) {
		perror("tcgetattr error, exit!");
		return -1;
	}

/*
LinuxのSerialのカノニカル｜非カノニカルの入力処理について

注意：
入力処理とは、Deviceから送信されたキャラクタを、readで読み出される前に、処理することを指す。

2種類の入力処理の中で、適切なものを選ぶべき。


「詳解UNIXプログラミング」本から：

ーーーーーーーーーーーーーーーー
種類１：　カノニカル入力処理
ーーーーーーーーーーーーーーーー
カノニカルモードは単純である。
プロセスで読み取りを行うと、端末ドライバは入力された行を返す。読み取りから戻るにはいくつかの条件がある。

条件１：指定したByte数を読み取ると戻る。一行を纏めて読む必要はない。
	行の一部を読んだ場合でも、情報を紛失することはない。
	次回の読み取りは、直前の読み取りの終了点から始まる。
条件２：行の区切りに出会うと読み取りから戻る。11.3節でカノニカルモードにおいては、NL、EOL、EOL2、EOFの文字を「行の終わり」と解釈することを述べた。
	更に、11.5節の述べたように、ICRNLを設定しIGNCRを設定してない場合、CR文字はNL文字と同様な動作をするため行を区切る。
	これらの5つの行区切りのうち、EOFは端末ドライバで処理した後廃棄される。残りの4つは行の最後の文字として呼び出し側に返される。
条件３：Signal捕捉して、関数が自動的に再Startしない場合にも、読み取りから戻る。



ーーーーーーーーーーーーーーーー
種類２：　非カノニカル入力処理
ーーーーーーーーーーーーーーーー
termios構造体のc_lflagのICANON flagをOffにすると、非カノニカルモードを指定出来る。
非カノニカルモードでは、入力データを行に纏めない。
ERASE、KILL、EOF、NL、EOL、EOL2、CR、REPRINT、STATUS、WERASEの特別な文字は処理されない。

既に述べたように、カノニカルモードは簡単である。Systemは一度に１行を返す。しかし、非カノニカルモードでは、
SystemはProcessにデータを返す時期をどのように知るのであろうか？
一度に１Byteを返すと、非常に大きなOverheadを伴う(勿論、一度に２Byteを返すと、Overheadは半分に減る)。
つまり、読み取りを始める前に、何Byte読めるか分からないため、Systemが常に複数のByteを返すことが出来ないのだ。

解決策は、
	指定したデータ量を読み取った場合、或は
	指定した時間が経過した場合に、戻るようにSystemに設定することである。
	
	これには、termios構造体の配列c_cc[]の中の２つの変数：MIN,TIME(index: VMIN,VTIME)を使う。
	MINは、readから戻るまで最小Byte数を指定する。
	TIMEは、データの到着の待ち時間を1/10秒単位で指定する。

	なので、MIN、TIMEを使う時には、４パターンがある：
	場合A： MIN>0,TIME>0
	場合B： MIN>0,TIME=0
	場合C： MIN=0,TIME>0
	場合D： MIN=0,TIME=0

               MIN>0                                 MIN=0
         +---------------------------------+---------------------------------------------+
         |pattern A:                       |  pattern C:                                 |
         |  Timerが切れる前にread()は      |    Timerが切れる前にread()は[1,nbytes]を返す|
TIME>0   |  [MIN,nbytes]を返す。           |    Timerが切れるとread()は0を返す。         |
         |  Timerが切れると[1,MIN]を返す。 |    (タイマーはreadタイマー)                 |
         |  Timerはバイトタイマー          |                                             |
         |  呼び出し側無期限Blockされる。  |                                             |
         +-------------------------------------------------------------------------------+
         |pattern B:                       |  pattenr D:                                 |
TIME=0   |  データがあればread()は         |                                             |
         |    [MIN,nbytes]を返す           |    read()は直ちに[0,nbytes]を返す           |
         | (呼び出し側は無期限Blockされる) |                                             |
         +---------------------------------+---------------------------------------------+



ーーーーーーーーーーーーーーーー
非同期入力
ーーーーーーーーーーーーーーーー
上記2つのモードは、更に、同期/非同期で使うことができるが、

Defaultは、入力が上手く行くまで、read()がBlockされる同期モードである。

非同期モードでは、read()は即座に終了し、
後で読み込みが完了した時に、プログラムにSignalが送られる。
このSignalは、Signal　Handlerを使って受け取るべき。

*/

	/* カノニカル入力処理をEnable  */
	//tio.c_lflag &= ~ICANON;
	tio.c_lflag |= ICANON;
	
	/*　CLOCAL:　これを設定すると、モデムの状態信号を無視する。これは通常、装置がLocalに接続されていることを意味する。
		     このflagを設定しないと、例えば、モデムが応答するまでは端末装置のopenがBlockされる  
	　　CREAD: 　これを設定すると、受信可能にし、文字を受け取れるようになる。
	　　CSIZE:   送受信におけるByteあたりのBit数を指定するMask。このサイズにはパリティBitは含まない。
		     値はCS5、CS6、CS7、CS8のいずれかであり、それぞれ１Byteあたり5,6,7,8 Bitを意味する。
	    
	*/
	tio.c_cflag |= CLOCAL | CREAD;	
	tio.c_cflag &= ~CSIZE;		/* 文字サイズのマスクは無し  */

	/*   */
	tio.c_cflag |= CS8;		/* データ長 */
	tio.c_cflag &= ~PARENB;		/* パリティなし */
	tio.c_cflag &= ~CSTOPB;		/* Stop Bit: 1 bit */

	cfsetospeed(&tio, B115200);
	cfsetispeed(&tio, B115200);
	
	tio.c_cc[VTIME] = 1;
	tio.c_cc[VMIN] = 0;

	/* now clean the line and activate the settings for the port */
	tcflush(gps_fd, TCIOFLUSH);

	if (tcsetattr(gps_fd, TCSANOW, &tio) != 0) {
		perror("tcgetattr error, exit!");
		return -1;
	}

	return 0;
}

int init_gps_chip()
{
	//
	return 0;
}

int main(void)
{
	int read_bytes, total_bytes = 0;
	char *start, *end;
	sigset_t block_mask;
	

	/* Register signal handler for SIGINT */
	if (-1 == register_signals()) {
		exit(EXIT_FAILURE);
	}

	/* Prepare signal blocking */
	if (-1 == sigemptyset(&block_mask) || -1 == sigaddset(&block_mask, SIGINT)) {
		perror("prepare signal blocking");
		exit(EXIT_FAILURE);
	}

	/* open serial port */
	gps_fd = open("/dev/ttymxc2", O_RDWR | O_NOCTTY );
	if (gps_fd < 0) {
		perror("can't open /dev/ttymxc2\n");
		return -1;
	}

	init_host_serial_port();
	init_gps_chip();

	printf("wait sentence...\n");
	while (1) {
		nmea_s *data;
		char buf[255];


		/* Unlock signal */
		sigprocmask(SIG_UNBLOCK, &block_mask, NULL);

		read_bytes = read(gps_fd, buffer + total_bytes, 20);
		if (-1 == read_bytes) {
			perror("read stdin");
			exit(EXIT_FAILURE);
		}
		if (0 == read_bytes) {
			sig_quit(SIGINT);
		}

		total_bytes += read_bytes;

		/* block signal */
		sigprocmask(SIG_BLOCK, &block_mask, NULL);


		/* find start (a dollar $ign) */
		start = memchr(buffer, '$', total_bytes);
		if (NULL == start) {
			total_bytes = 0;
			continue;
		}

		/* find end of line */
		end = memchr(start, NMEA_END_CHAR_1, total_bytes - (start - buffer));
		if (NULL == end || NMEA_END_CHAR_2 != *(++end)) {
			continue;
		}

		/* handle sentence data */
		data = nmea_0183_parse(start, end - start + 1, 0);	/* main */
		if (NULL != data) {
			if (0 < data->errors) {
				printf("WARN: The sentence struct contains parse errors!\n");
			}

			nmea_free(data);
			printf("----------------------------------------->\n");
		}

		/* buffer empty? */
		if (end == buffer + total_bytes) {
			total_bytes = 0;
			continue;
		}

		/* copy rest of buffer to beginning */
		if (buffer != memmove(buffer, end, total_bytes - (end - buffer))) {
			total_bytes = 0;
			continue;
		}

		total_bytes -= end - buffer;
	}

	close(gps_fd);
	return 0;
}


/*************************** "GPGSV" sentence parser ********************************/
int gsv_parser_init(nmea_parser_s *parser)
{
	/* Declare what sentence type to parse */
	NMEA_PARSER_TYPE(parser, NMEA_GPGSV);
	NMEA_PARSER_PREFIX(parser, "GPGSV");
	return 0;
}

int gsv_parser_allocate_data(nmea_parser_s *parser)
{
	parser->data = malloc(sizeof (nmea_gpgsv_s));
	if (NULL == parser->data) {
		return -1;
	}
	return 0;
}

int gsv_parser_set_default(nmea_parser_s *parser)
{
	memset(parser->data, 0, sizeof (nmea_gpgsv_s));
	return 0;
}

int gsv_parser_free_data(nmea_s *data)
{
	free(data);
	return 0;
}

int gsv_parser_parse(nmea_parser_s *parser, char *value, int val_index)
{
	//TODO...まだ作成中。

	nmea_gpgsv_s *data = (nmea_gpgsv_s *) parser->data;
	
	switch (val_index) {
	case NMEA_GPGSV_SENTENCES:
		break;

	case NMEA_GPGSV_SENTENCE_NUMBER:
		break;

	case NMEA_GPGSV_SATELLITES:
		g_info.satinfo.inview = atoi(value);
		printf("GP inview=%d\n", g_info.satinfo.inview);
		break;

	case NMEA_GPGSV_PRN1:
	case NMEA_GPGSV_PRN2:
	case NMEA_GPGSV_PRN3:
	case NMEA_GPGSV_PRN4:
		printf("GP prn=%d\n", atoi(value));
		break;

	case NMEA_GPGSV_ELEVATION1:
	case NMEA_GPGSV_ELEVATION2:
	case NMEA_GPGSV_ELEVATION3:
	case NMEA_GPGSV_ELEVATION4:
		printf("GP ele=%d\n", atoi(value));
		break;

	case NMEA_GPGSV_AZIMUTH1:
	case NMEA_GPGSV_AZIMUTH2:
	case NMEA_GPGSV_AZIMUTH3:
	case NMEA_GPGSV_AZIMUTH4:
		printf("GP azi=%d\n", atoi(value));
		break;

	case NMEA_GPGSV_SNR1:
	case NMEA_GPGSV_SNR2:
	case NMEA_GPGSV_SNR3:
	case NMEA_GPGSV_SNR4:
		printf("GP snr=%d\n", atoi(value));
		break;

	default:
		break;
	}

	return 0;
}

/*************************** "GPRMC" sentence parser ********************************/
int rmc_parser_init(nmea_parser_s *parser)
{
	NMEA_PARSER_TYPE(parser, NMEA_GPRMC);
	NMEA_PARSER_PREFIX(parser, "GPRMC");
	return 0;
}

int rmc_parser_allocate_data(nmea_parser_s *parser)
{
	parser->data = malloc(sizeof (nmea_gprmc_s));
	if (NULL == parser->data) {
		return -1;
	}

	return 0;
}

int rmc_parser_set_default(nmea_parser_s *parser)
{
	memset(parser->data, 0, sizeof (nmea_gprmc_s));
	return 0;
}

int rmc_parser_free_data(nmea_s *data)
{
	free(data);
	return 0;
}

int rmc_parser_parse(nmea_parser_s *parser, char *value, int val_index)
{
	nmea_gprmc_s *data = (nmea_gprmc_s *) parser->data;

	switch (val_index) {
	case NMEA_GPRMC_TIME:
		if (-1 == nmea_time_parse(value, &data->time)) {
			return -1;
		}
		break;

	case NMEA_GPRMC_LATITUDE:
		if (-1 == nmea_position_parse(value, &data->latitude)) {
			return -1;
		}
		break;

	case NMEA_GPRMC_LATITUDE_CARDINAL:
		data->latitude.cardinal = nmea_cardinal_direction_parse(value);
		if (NMEA_CARDINAL_DIR_UNKNOWN == data->latitude.cardinal) {
			return -1;
		}
		break;

	case NMEA_GPRMC_LONGITUDE:
		if (-1 == nmea_position_parse(value, &data->longitude)) {
			return -1;
		}
		break;

	case NMEA_GPRMC_LONGITUDE_CARDINAL:
		data->longitude.cardinal = nmea_cardinal_direction_parse(value);
		if (NMEA_CARDINAL_DIR_UNKNOWN == data->longitude.cardinal) {
			return -1;
		}
		break;

	case NMEA_GPRMC_DATE:
		if (-1 == nmea_date_parse(value, &data->time)) {
			return -1;
		}
		break;

	case NMEA_GPRMC_SPEED:
		data->speed = atof(value);
		break;

	default:
		break;
	}

	return 0;
}

/*************************** sentence parser load/unload ********************************/
nmea_parser_module_s* nmea_init_parser(char *sentence)
{
	nmea_parser_module_s *parser;
	init_f init;

	/* Allocate parser struct */
	parser = malloc(sizeof (nmea_parser_module_s));
	if (NULL == parser) {
		return (nmea_parser_module_s *) NULL;
	}

	if (!strcmp(sentence, support_sentence[0])) {
		init = gsv_parser_init;
		parser->allocate_data = gsv_parser_allocate_data;
		parser->set_default = gsv_parser_set_default;
		parser->free_data = gsv_parser_free_data;
		parser->parse = gsv_parser_parse;

		if (-1 == init((nmea_parser_s *) parser)) {
			return (nmea_parser_module_s *) NULL;
		}
	}

	if (!strcmp(sentence, support_sentence[1])) {
		init = rmc_parser_init;
		parser->allocate_data = rmc_parser_allocate_data;
		parser->set_default = rmc_parser_set_default;
		parser->free_data = rmc_parser_free_data;
		parser->parse = rmc_parser_parse;

		if (-1 == init((nmea_parser_s *) parser)) {
			return (nmea_parser_module_s *) NULL;
		}
	}

	return parser;
}

int nmea_0183_load_parsers()
{
	int i;
	nmea_parser_module_s *parser;
	parsers_no = ARRAY_LENGTH(support_sentence);	/* GSV, RMC */

	/* allocate g_parsers array */
	g_parsers = malloc((sizeof (nmea_parser_module_s *)) * parsers_no);
	if (NULL == g_parsers) {
		return -1;
	}
	memset(g_parsers, 0, (sizeof (nmea_parser_module_s *)) * parsers_no);

	i = parsers_no;
	while (i-- > 0) {
		parser = nmea_init_parser(support_sentence[i]);
		if (NULL == parser) {
			return -1;
		}
		g_parsers[i] = parser;
		printf("add sentence parser type: %d\n", parser->parser.type);
	}

	return parsers_no;
}

void nmea_0183_unload_parsers()
{
	int i;

	for (i = 0; i < parsers_no; i++) {
		free(g_parsers[i]);
	}

	free(g_parsers);
}

void __attribute__ ((constructor)) nmea_0183_init(void);
void nmea_0183_init()
{
	nmea_0183_load_parsers();
}

void __attribute__ ((destructor)) nmea_0183_exit(void);
void nmea_0183_exit()
{
	nmea_0183_unload_parsers();
}
