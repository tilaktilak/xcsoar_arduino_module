#include <avr/interrupt.h>
#include <avr/pgmspace.h>
#include <avr/io.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <math.h>

#ifndef F_CPU
#define F_CPU 800000000UL
#endif

#ifndef BAUD
#define BAUD 38400
#endif
#include <util/setbaud.h>

#include "i2c_master.h"
#include "bmp280.h"
#include "softuart.h"
#include "kalman.h"
/* http://www.cs.mun.ca/~rod/Winter2007/4723/notes/serial/serial.html */
void uart_init(void) {
    UBRR0H = UBRRH_VALUE;
    UBRR0L = UBRRL_VALUE;

#if USE_2X
    UCSR0A |= _BV(U2X0);
#else
    UCSR0A &= ~(_BV(U2X0));
#endif

    UCSR0C = _BV(UCSZ01) | _BV(UCSZ00); /* 8-bit data */ 
    UCSR0B = _BV(RXEN0) | _BV(TXEN0);   /* Enable RX and TX */    
    UCSR0C |= _BV(RXCIE0);
}

uint8_t flag_volume_recv = 0;
uint8_t buf[12]={0};

ISR(USART_RX_vect){
    uint8_t data = UDR0;
    static uint8_t count = 0;
    if(count == 0){
        if(data == '$'){
            buf[count++]=data;
        }
        else{count = 0;}
    }
    else if(count == 1){
        if(data == 'B'){
            buf[count++]=data;
        }
        else{count = 0;}
    }
    else if(count == 2){
        if(data == 'V'){
            buf[count++]=data;
        }
        else{count = 0;}
    }
    else if(count == 3){
        if(data == ' '){
            buf[count++]=data;
        }
        else{count = 0;}
    }
    else if(count >3){
        if(data == '*'){
            // END !
            flag_volume_recv = 1;
            count = 0;
        }
    }
    else{
        count = 0;
    }
}   

int uart_putchar(char c, FILE *stream) {
    loop_until_bit_is_set(UCSR0A, UDRE0);
    UDR0 = c;
    return 0;
}

char uart_getchar(FILE *stream) {
    loop_until_bit_is_set(UCSR0A, RXC0);
    return UDR0;
}
FILE uart_output = FDEV_SETUP_STREAM(uart_putchar, NULL, _FDEV_SETUP_WRITE);

#define TIMER_FREQ_HZ   1

//const _Accum dt = powf(2,16)*(1.f/8E6);
_Accum dt = 0.0f;
// Count until 8192
_Accum time = 0.f;

volatile long millis;

void timer1_init(void){
    // TIMER 1 Config for timing while 1
    TCCR1A = 0;
    TCCR1B = 0;
    TCCR1C = 0;
    TCCR1B |=(1<<CS10) ;//| (1<<CS11);//prescaler=8
    //OCR1AL = 0b00000000;
    //OCR1AH = 0b00100000;
}

void timer2_init(void){
    // TIMER 2 Config for PWM tone
    TCCR2A = (1<<WGM21) | (1<<WGM20);
    TCCR2B = (1<<CS20) | (0<<CS21) | (1<<CS22) |(1<<WGM22);
    OCR2A = 0x00;
    //OCR2B = 0x00;
    TIMSK2 = (1<<OCIE2A);
    //TIMSK2 |= (1<<OCIE2B);


    // Init A3 pin (PC3)
    DDRC |= (1<<DDC3);
    PORTC &= ~(1<<PORTC3);
}
// Fmin = 250Hz
void timer2_set_freq(_Accum freq){
    cli();
    OCR2A = (uint8_t)((1./freq)*8E6/128.f);
    // PERIOD = 1s -> 8E6/128f
    // PERIOD = 0.5s -> 
    sei();
}

volatile int32_t duration = 0;
volatile int32_t tim2_period = 0;
volatile uint8_t mute = 0;
volatile uint8_t state = 0;

void timer2_set_duration(_Accum d_sec){
    cli();
    duration = (int)(d_sec/2.f*(8E6/128.f)/(_Accum)OCR2A);
    tim2_period = duration;//Launch new period
    sei();
}

void timer2_set_volume(_Accum volume){
    cli();
    OCR2B = (uint8_t)((OCR2A/2)*(volume/100.f));// Pourcent of OCR2A

    sei();
}

_Accum altitude = 0.f;

int parse_nmea(uint8_t c){
    static int i = 0;
    static int i_comma = 0;
    static int nb_comma = 0;
    static uint8_t alt[10]={'0'};
    if( i==0){
        if(c == '$'){i++;}
    }
    else if(i==1){
        if(c == 'G'){i++;}
        else{i=0;}
    }
    else if(i==2){
        if(c == 'P'){i++;}
        else{i=0;}
    } 
    else if(i==3){
        if(c == 'G'){i++;}
        else{i=0;}
    } 
    else if(i==4){
        if(c == 'G'){i++;}
        else{i=0;}
    } 
    else if(i==5){
        if(c == 'A'){i++;}
        else{i=0;}
    } 
    else if(i==6){
        if(c == ','){i++;}
        else{i=0;}
    }
    else if(i>6){
        i++;
        if(c==','){nb_comma++;}
        if(nb_comma==8){
            i_comma++;
            if(i_comma>1){
                alt[i_comma-2]=c;
            }
        } 
        else if(nb_comma==9){
            alt[(i_comma<sizeof(alt)?i_comma:sizeof(alt))] = '\0';
            // Here set altitude global variable
            altitude = atof((const char*)alt);

            i_comma=0;
            nb_comma = 0;
            i=0;
        }
    }
    else{
        i = 0;
    }
    return 0;
}

volatile uint8_t tone_done = 0;


int main(void)
{

    uart_init();
    softuart_init();
    softuart_turn_rx_on(); /* redundant - on by default */  
    i2c_init();

    sei();


    stdout = &uart_output;

    InitBMP280();

    _Accum alt      = 0.f;
    _Accum rate     = 0.f;
    //_Accum ralt     = 0.f;
    //_Accum old_alt  = 0.f;

    _Accum freq = 300.f;
    _Accum const low_level = -0.3f;
    _Accum const high_level = 0.2f;
    _Accum const low_gain = -50.f;
    _Accum const low_offset = 300.f;
    _Accum const high_gain = 150.f;
    _Accum const high_offset = 1100.f;


    long int const prs_count_max = 2; // Period = 256/8Mhz, to 50Hz/20ms => 20ms/
    int prs_count = 0;


    timer1_init();

    timer2_init();
    timer2_set_freq(250.f);
    timer2_set_duration(1.f);
    //timer2_set_volume(0.f);

    alt = AltitudeBMP280();
    kalman_init(alt);
    char c = ' ';


    for (;;) {
        while((TIFR1 & (1<<TOV1))!=(1<<TOV1)){// Wait until flag set


		if(new_nmea_line){
			while(softuart_kbhit()) {
				c = softuart_getchar();
				putchar(c);
				if(c == '\n'){
					new_nmea_line = 0;
					break;
				}
			}
		}


        }
        TIFR1 |= (1 << TOV1);

        if(++prs_count>=prs_count_max ){
            prs_count = 0;
            uint32_t inttp = (uint32_t)press;// Global var set by AltitudeBMP
            printf("PRS %05lx\r\n",(unsigned long int)inttp);
        }

        time += dt;

        alt = AltitudeBMP280();
        if(alt>0.f && alt<20000.f){
            kalman_predict(0.01f);
        }
        kalman_update(alt);
        rate = X[1];

        if(tone_done==1){
            if(rate < low_level){
                mute = 0;
                freq = low_gain*fabs(rate) + low_offset;
            }
            else if(rate > high_level){
                mute = 0;
                freq = rate * high_gain + high_offset;
            }
            else {
                mute = 1;// Mute
            }
            // Constraint freq
            if(freq<250.f)freq = 250.f;

            timer2_set_freq(freq);
            timer2_set_duration(500.f/freq);
            //timer2_set_volume(10.f);
            tone_done = 0;
        }


    }	
    return 0; /* never reached */
}


void toggle_PC3(void){
    if(!mute){
        if(PORTC&(1<<PORTC3)){
            PORTC &= (0<<PORTC3);
        }
        else{
            PORTC |= (1<<PORTC3);
        }
    }
    else{
        PORTC &= (0<<PORTC3);
    }
}

// Time counter will be in Timer2 IRQ A
// It's called every 16us ( 8Mhz / 128 presc)
// If we want to convert it to ms, just do a
// scale constant multiplier
//
// To insure no overflow, maybe restart counter 
// every time data is used (use it as a dt value)
//
ISR(TIMER2_COMPA_vect){
    millis++;
    if(state == 0){// Noisy half period
        toggle_PC3();
        //if(!mute) PORTC |= (1<<PORTC3);
        if((--tim2_period)<=0) state = 1;
    }
    if(state == 1){// Mute half period
        if(++tim2_period>=duration){
            state = 0;
            tone_done = 1;
        }
    }
}

/*
ISR(TIMER2_COMPB_vect){
    if(state == 0){
        if(!mute)PORTC &=(0<<PORTC3);
    }
}*/
