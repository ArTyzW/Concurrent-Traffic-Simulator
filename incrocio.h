#ifndef INCROCIO_H
#define INCROCIO_H

#include <stdio.h>
#include <unistd.h>
#include <stdlib.h>
#include <semaphore.h>
#include <sys/time.h>

#define NESSUNA_AUTO     -1
#define NUM_STRADE        4

static inline int GetDistanceFromStreet(int iStreet, int iDirezione) {
    if(iDirezione == 0)
        iDirezione = 4;
    return(iDirezione - iStreet);
}

static inline int StreetOnTheLeft(int iMyStrada, int iDistance) {
     if(iDistance < 1 || iDistance > 3) {
         fprintf(stderr, "DISTANZA ERRATA: %d!!!\n", iDistance);
         exit(EXIT_FAILURE);
     }
     //printf("%d: MyStrada = %d, incremento = %d\n", getpid(), iMyStrada, iDistance);
     return (iMyStrada + iDistance) % NUM_STRADE;
 }

/**********************************************************************************************************************/
// Function that randomly extracts the street into which the car coming from the street given as an argument must turn.
// The extracted street is returned to the caller as 0-based index (from 0 to (NUM_STRADE - 1))
/**********************************************************************************************************************/
static inline int EstraiDirezione(int iMyStreet) {
    int iDirezione = iMyStreet;
    struct timeval tv;
    long l;

    while(iDirezione == iMyStreet) {
        gettimeofday(&tv, NULL);
        l = tv.tv_usec;
        l = l % NUM_STRADE;
        iDirezione = (int)l;
    }

    return iDirezione;
}


/**********************************************************************************************************************/
// This function simulates traffic priorities between cars according to the highway code... more or less...
// It returns the index of the car allowed to pass, one by one.
//
// Accepts as argument an array of 4 integer elements. Each element contains the street index into which
// the i-th car must turn, or -1 if no car comes from the i-th street. That is:
// - the first element corresponds to car 0, coming from street 0, and contains the street index where car 0 turns
//   (or -1 if no car comes from street 0)
// - the second element corresponds to car 1, coming from street 1, and contains the street index where car 1 turns
//   (or -1 if no car comes from street 1)
// - and so on
//
// For example: an array with values [3, -1, 1, 2] means that car 0 (first array element), coming from street 0,
// must turn into street 3; no car comes from street 1; car 2, coming from street 2, must turn into street 1; and so on.
//
// In this example, if called repeatedly, the function returns in order: 0, 3, 2
/**********************************************************************************************************************/
static inline int GetNextCar(int *piDirezioni) {
    int iAuto = NESSUNA_AUTO;

    for(int i = 0; i < NUM_STRADE; i++) {
        int iMyStreet = i;

        if(piDirezioni[iMyStreet] != NESSUNA_AUTO) {
            if(piDirezioni[iMyStreet] == StreetOnTheLeft(iMyStreet, 1)) {
                // The i-th car immediately turns RIGHT
                iAuto = i;
                break;
            }
            else {
                if(piDirezioni[StreetOnTheLeft(iMyStreet, 1)] != NESSUNA_AUTO)
                    // The i-th car does not have a clear RIGHT side
                    continue;
                else {
                    // The i-th car has a clear RIGHT side.
                    // Check if, while turning, the i-th car will find on its RIGHT side
                    // the car coming from the opposite street
                    int iStradaDiFronte = StreetOnTheLeft(iMyStreet, 2);
                    if(piDirezioni[iStradaDiFronte] == NESSUNA_AUTO) {
                        iAuto = i;
                        break;
                    }
                    else {
                        int iDoveVaDiFronte = piDirezioni[iStradaDiFronte], iDoveVaIesima = piDirezioni[iMyStreet];
                        if(GetDistanceFromStreet(iMyStreet, iDoveVaIesima) <= GetDistanceFromStreet(iMyStreet, iStradaDiFronte) 
                           ||
                           (GetDistanceFromStreet(iMyStreet, piDirezioni[iStradaDiFronte]) < GetDistanceFromStreet(iMyStreet, piDirezioni[iMyStreet]))) {
                            iAuto = i;
                            break;
                        }
                    }
                }
            }
        }
    }

    if(iAuto == NESSUNA_AUTO) {
        for(int i = 0; i < NUM_STRADE; i++) {
            if(piDirezioni[i] != NESSUNA_AUTO) {
                iAuto = i;
                break;
            }
        }
    }

    return iAuto;
}

#endif // INCROCIO_H