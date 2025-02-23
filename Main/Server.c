#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>
#include <ctype.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <pthread.h>
#include <stdbool.h>
#include <getopt.h>
#include <signal.h>
#include "../CommsHandler/CommsHandler.h"
#include "../Utility/Utility.h"
#include "../GameState/GameState.h"

//Indice parametri
#define ADDR_INDEX 1
#define PORT_INDEX 2

//Utilizzo questa struct per definire i possibili parametri con double-dash e il relativo valore per lo switch
static struct option long_options[] =
{
    {"matrici", required_argument, NULL, 'm'},
    {"durata", required_argument, NULL, 't'},
    {"seed", required_argument, NULL, 's'},
    {"diz", required_argument, NULL, 'd'},
    {NULL, 0, NULL, 0} //Possibile aggiunti di altri parametri che gestisco nello switch
};

//Dichiaro le funzioni
void* startServer(); //Funzione del thread per avviare il server
void timeTick(); //Funzione callback per SIGALARM
void* scorer(); //Funzione del thread per gestire gli score
void* handlePlayer(void* playerArg); //Funzione del thread per gestire una nuova connessione
void* playerAsync(void* playerArg); //Funzione del thread per inviare in modo asincrono messaggi ai client
void* playerSync(void* playerArg); //Funzione del thread per leggere i messaggi dai client

void getOptionalParameters(int argc, char** argv); //Funzione per leggere i parametri opzionali
void exitBeforeStart(char* message); //Funzione per uscire liberando la memoria prima dell'inizio del game
void exitServerThreadWithMessage(char* message); //Funzione per terminare il thread del server inducendo il processo di terminazione previsto del programma

//Struct condivisa che contiene tutte le informazioni del gioco
static GameInfo* gameInfo = NULL;

//Variabili di condizione per gestire la lista degli score
pthread_mutex_t score_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t score_added_cond = PTHREAD_COND_INITIALIZER;
pthread_cond_t score_ready_cond = PTHREAD_COND_INITIALIZER;
//Dichiarazione variabile di condizione per gestire la lista di giocatori (definita come extern in GameState.h)
pthread_mutex_t players_mutex = PTHREAD_MUTEX_INITIALIZER;

int main(int argc, char** argv)
{
    //Gestione del segnale SIGINT per terminare
    sigset_t set;
    sigemptyset(&set); //Svuoto il set dei segnali masked
    sigaddset(&set, SIGINT); //Aggiungi SIGINT ai segnali masked
    pthread_sigmask(SIG_SETMASK, &set, NULL);

    //Validazione parametri obbligatori
    if(argc-1 < 2) exitWithMessage("Nome host e numero porta sono obbligatori"); //Controllo che ci siamo almeno i primi 2 parametri (argc-1 per il nome del file)

    in_addr_t addr;
    int port;
    if(!validatePort(argv[PORT_INDEX], &port)) exitWithMessage("Numero di porta non corretto"); //Controllo con una funzione se la porta inserita è valida, se non lo è interrompo
    if(!validateAddr(argv[ADDR_INDEX], &addr)) exitWithMessage("Hostname o indirizzo IPV4 non valido"); //Controllo con una funzione se l'indirizzo è corretto, se non lo è interrompo

    //Inizializzazione struttura di gioco
    gameInfo = initGameInfo(addr, port); //Inizializzo la struct che contiene le informazioni del gioco
    getOptionalParameters(argc, argv); //Aggiorno la struct con i parametri opzionali
    srand(gameInfo->seed); //Inizializzo srand con il seed time(0) o con quello inserito come parametro

    //Carico il dizionario da file
    gameInfo->dictionary = loadDictionary(gameInfo->dictionaryFile);
    if(gameInfo->dictionary == NULL) exitBeforeStart("Errore nella lettura del dizionario");

    //Inizializzazione sessione di gioco
    if(!initGameSession(gameInfo)) exitBeforeStart("Errore nella lettura della matrice dal file");

    //Creazione di un thread che gestisce il server
    pthread_t server_thread;
    if (pthread_create(&server_thread, NULL, startServer, NULL) != 0) {
        freeGameMem(gameInfo); //Se non riesco a creare il thread libero la memoria e chiudo il processo
        exitWithMessage("Errore creazione thread server");
    }

    int sig;
    sigwait(&set, &sig); //Attendo il segnali SIGINT
    alarm(0); //Cancello un eventuale alarm in corso
    if(gameInfo->currentSession != NULL) gameInfo->currentSession->gamePhase = Off;
    printf("\nInterruzione del gioco in corso...\n");
    fflush(stdout);

    //Chiusura del socket che induce l'interruzione del server_thread (con shutdown mi assicuro che la accept venga interrotta)
    shutdown(gameInfo->serverSocket, SHUT_RDWR); //Non eseguo controlli sul risultato perché sto chiudendo il programma
    close(gameInfo->serverSocket); //Non eseguo controlli sul risultato perché sto chiudendo il programma
    pthread_join(server_thread, NULL); //Attendo la chiusura del server_thread (se è ancora attivo)

    freeGameMem(gameInfo); //Libero tutta la memoria occupata dalle strutture di gioco
}


void* startServer() //Funzione del thread per avviare il server
{
    struct sockaddr_in server_addr, client_addr;

    //Creo il socket del server
    if(syscall_fails_get(gameInfo->serverSocket, socket(AF_INET, SOCK_STREAM, 0))) {
        exitServerThreadWithMessage("Errore durante la creazione server socket");
    }

    //Imposto il server in modo da poter riutilizzare la porta e l'indirizzo appena chiuso
    int opt=1;
    if(setsockopt(gameInfo->serverSocket, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt))) exitServerThreadWithMessage("Errore durante il setup del server socket");
    if(setsockopt(gameInfo->serverSocket, SOL_SOCKET, SO_REUSEPORT, &opt, sizeof(opt))) exitServerThreadWithMessage("Errore durante il setup del server socket");

    //Imposto i dati per la connessione
    server_addr.sin_family = AF_INET;
    server_addr.sin_port = htons(gameInfo->serverPort);
    server_addr.sin_addr.s_addr = gameInfo->serverAddr;

    //Avvio il server e rimango in attesa di MAX_CLIENTS
    if(syscall_fails(bind(gameInfo->serverSocket, (struct sockaddr *) &server_addr, sizeof(server_addr))) || syscall_fails(listen(gameInfo->serverSocket, MAX_CLIENTS))){
        exitServerThreadWithMessage("Errore durante l'avvio del server, riprova");;
    }

    printf("Server avviato con successo\n");
    gameInfo->currentSession->gamePhase = Paused; //Se il server è stato creato avvio il gioco in pausa

    //Mask e handler per intercettare SIGALARM
    struct sigaction act;
    sigset_t set;
    sigemptyset(&set); //Svuoto il set dei segnali masked
    sigaddset(&set, SIGALRM); //Aggiungo SIGALRM ai segnali masked
    act.sa_handler = &timeTick; //Handler personale
    act.sa_flags = SA_RESTART; //Restart delle system call bloccate
    act.sa_mask = set;
    sigaction(SIGALRM, &act, NULL);

    alarm(1); //Avvio il gioco tramite il segnale SIGALARM intercettato da timeTick

    socklen_t client_addr_len = sizeof(client_addr);
    while(true){
        //Accetto una nuova connessione da un client
        int fd_client;
        //Se il main ha chiuso il canale di comunicazione la accept fallisce ed esco dal thread
        if(syscall_fails_get(fd_client, accept(gameInfo->serverSocket, (struct sockaddr *) &client_addr, &client_addr_len))) break;

        //Creo un nuovo giocatore
        Player* player = createPlayer(fd_client);

        //Lascio gestire il player a un nuovo thread
        if (pthread_create(&player->thread[Main], NULL, handlePlayer, (void*) player) != 0)
        {
            //Se non riesco a creare un thread per un client continuo a giocare ma avviso con un errore e libero la memoria del player
            freeStringArray(player->foundWords);
            free(player);
            printf("Errore creazione thread client");
        }
    }
    pthread_exit(NULL);
}


void timeTick() //Funzione callback per SIGALARM
{
    if(gameInfo == NULL || gameInfo->currentSession == NULL) return;
    if(gameInfo->currentSession->gamePhase == Off) return;

    gameInfo->currentSession->timeToNextPhase--; //Decremento il counter del tempo
    if(gameInfo->currentSession->timeToNextPhase <= 0)
    {
        //Aggiorno la fase di gioco e la durata relativa
        if(gameInfo->currentSession->gamePhase == Paused)
        {
            gameInfo->currentSession->gamePhase = Playing;
            gameInfo->currentSession->timeToNextPhase = gameInfo->gameDuration;
        }
        else
        {
            gameInfo->currentSession->gamePhase = Paused;
            gameInfo->currentSession->timeToNextPhase = PAUSE_TIME;
        }

        if(gameInfo->currentSession->gamePhase == Playing) generateMatrix(gameInfo); //Se il gioco è ricominciato creo la nuova matrice

        pthread_t scorer_thread;
        pthread_create(&scorer_thread, NULL, scorer, NULL); //Creo il thread che si occupa di gestire gli score
        pthread_join(scorer_thread, NULL); //Attendo che finisca di gestire gli score

        if(gameInfo->currentSession->gamePhase == Playing) //Se il gioco è ricominciato resetto gli score dopo che il thread scorer ha terminato il suo lavoro
        {
            freeScoreArray(gameInfo->currentSession->scores);
            gameInfo->currentSession->scores = createScoreArray();
        }
    }
    alarm(1); //Riavvio il counter
}


void* scorer() //Funzione del thread per gestire gli score
{
    if(gameInfo->currentSession == NULL || gameInfo->currentSession->gamePhase == Off) pthread_exit(NULL);

    pthread_mutex_lock(&players_mutex); //Evito che altri player possano unirsi al game durante il rilascio dei punti

    //Invio il messaggio asincrono ai player
    for(int i = 0; i<MAX_CLIENTS; i++){
        if(gameInfo->currentSession->players[i] != NULL && gameInfo->currentSession->players[i]->bRegistered)
        {
            if(gameInfo->currentSession->gamePhase == Playing) { //Se sto iniziando una nuova partita
                freeStringArray(gameInfo->currentSession->players[i]->foundWords); //Elimino le parole trovate
                gameInfo->currentSession->players[i]->foundWords = createStringArray(); //Ricreo l'array per il prossimo round
                gameInfo->currentSession->players[i]->score = 0; //Resetto lo score dei giocatori
            }
            pthread_kill(gameInfo->currentSession->players[i]->thread[Async], SIGUSR1); //Invio ai thread dei player il segnale per avviare la lettura dei punteggi o l'invio della matrice
        }
    }

    if(gameInfo->currentSession->gamePhase == Playing){ //Se non devo mandare gli score esco (cioè è appena finita la pausa e sono in game)
        pthread_mutex_unlock(&players_mutex); //Sblocco il lock dei player
        pthread_exit(NULL);
    }

    pthread_mutex_lock(&score_mutex); //Blocco il lock dello score condiviso con i write_player threads
    while(gameInfo->currentSession->scores->size < gameInfo->currentSession->numPlayers) pthread_cond_wait(&score_added_cond, &score_mutex); //Aspetto finché tutti i thread non hanno aggiornato la queue

    char* newCSV = malloc(sizeof(char)); //Alloco un byte temporaneo
    newCSV[0] = '\0'; //Ci metto il \0

    //Bubble sort per ordinare gli score per la classifica
    ScoreData tempScore;
    for (int i = 0; i < gameInfo->currentSession->scores->size - 1; i++) {
        for (int j = 0; j < gameInfo->currentSession->scores->size - i - 1; j++) {
            if (gameInfo->currentSession->scores->scores[j].score < gameInfo->currentSession->scores->scores[j + 1].score) { //Se il precedente è minore del successivo
                tempScore = gameInfo->currentSession->scores->scores[j]; //Assegno il primo a una variabile temporanea
                gameInfo->currentSession->scores->scores[j] = gameInfo->currentSession->scores->scores[j + 1]; //Scambio il primo con il secondo
                gameInfo->currentSession->scores->scores[j + 1] = tempScore; //Scambio il secondo con quello temporaneo
            }
        }
    }

    int lastScore = 0; //Variabile per controllare se la partita è patta
    bool bIsEven = gameInfo->currentSession->scores->size > 1; //Se c'è solo un giocatore non può essere patta
    for(int i=0; i < gameInfo->currentSession->scores->size; i++)
    {
        //Controllo se la partita è patta
        if(gameInfo->currentSession->scores->scores[i].score != lastScore) bIsEven = false; //Se trovo uno score differente non è patta
        lastScore = gameInfo->currentSession->scores->scores[i].score; //Aggiorno l'ultimo score trovato

        //Uso la snprintf con numero di byte scritti a 0 per sapere di quanta memoria ho bisogno
        int needed_size = snprintf(NULL, 0, "%s, %d\n", gameInfo->currentSession->scores->scores[i].playerName, gameInfo->currentSession->scores->scores[i].score);
        char* newScoreLine = malloc(needed_size + sizeof(char)); //Alloco la memoria + 1 char per il \0
        snprintf(newScoreLine, needed_size + sizeof(char), "%s, %d\n", gameInfo->currentSession->scores->scores[i].playerName, gameInfo->currentSession->scores->scores[i].score); //Scrivo la string nel buffer

        size_t currentTextLen = strlen(newCSV);
        size_t newSize = currentTextLen + needed_size + sizeof(char); //Calcolo la lunghezza per aggiornare il testo totale (+1 char per il \0)

        char* tempAlloc = malloc(newSize); //Alloco un blocco nuovo
        strcpy(tempAlloc, newCSV); //Trasferisco il testo corrente nel nuovo spazio di allocazione
        free(newCSV); //Libero il vecchio spazio di memoria
        newCSV = tempAlloc; //Aggiorno il puntatore al nuovo spazio di memoria

        strcat(newCSV, newScoreLine); //Concateno la nuova stringa con il testo totale
        free(newScoreLine); //Libero la stringa temporanea
    }

    //Scelta del vincitore (scrivo pari o il nome del vincitore nella struct)
    if(bIsEven)
        copyString(&gameInfo->currentSession->scores->winner, "PARI");
    else if(gameInfo->currentSession->scores->size > 0)
        copyString(&gameInfo->currentSession->scores->winner, gameInfo->currentSession->scores->scores[0].playerName);

    gameInfo->currentSession->scores->textCSV = newCSV; //Aggiorno il puntatore nella struct

    pthread_cond_broadcast(&score_ready_cond); //Sveglio tutti i thread write_player in attesa di mandare il messaggio punti finale
    pthread_mutex_unlock(&score_mutex); //Sblocco il lock dello score
    pthread_mutex_unlock(&players_mutex); //Sblocco il lock dei player
    pthread_exit(NULL);
}


void* handlePlayer(void* playerArg) //Funzione del thread per gestire una nuova connessione
{
    Player* player = (Player*) playerArg; //Casting del parametro a Player*

    pthread_create(&player->thread[Sync], NULL, playerSync, playerArg); //Creo il thread che si occupa di gestire i messaggi sincroni con il client
    pthread_create(&player->thread[Async], NULL, playerAsync, playerArg); //Creo il thread che si occupa di inviare i messaggi asincroni al client

    pthread_join(player->thread[Sync], NULL); //Attendo la chiusura del client di lettura messaggi
    player->bRegistered = false; //Se si chiude il thread di lettura il player non è più registrato

    pthread_kill(player->thread[Async], SIGUSR1); //Invio un segnale per far chiudere il thread di asincrono
    pthread_join(player->thread[Async], NULL); //Aspetto che si chiuda

    deletePlayer(gameInfo, player); //Elimino la memoria occupata dal player
    pthread_exit(NULL);
}


void* playerAsync(void* playerArg) //Funzione del thread per inviare in modo asincrono messaggi ai client
{
    Player* player = (Player*) playerArg; //Casting del parametro a Player*

    //Creo la mask per il segnale SIGUSR1
    sigset_t set; int sig;
    sigemptyset(&set); //Svuoto il set
    sigaddset(&set, SIGUSR1); //Aggiungo SIGUSR1 al set dei masked
    pthread_sigmask(SIG_SETMASK, &set, NULL);

    while (true)
    {
        sigwait(&set, &sig);  //Aspetto un SIGUSR1 per rilasciare i punti
        if(!player->bRegistered) break; //Se il segnale arriva quando il player sta uscendo esco
        if(gameInfo->currentSession->gamePhase == Paused) //Se il gioco sta terminando (cioè è appena finita la partita)
        {
            pthread_mutex_lock(&score_mutex); //Blocco il lock dello score condiviso con lo scorer i gli altri player_write threads

            addScoreToArray(gameInfo->currentSession->scores, player->name, player->score); //Aggiungo alla lista lo score del player gestito da questo thread

            pthread_cond_signal(&score_added_cond); //Segnalo allo scorer che è stato aggiunto un nuovo valore
            pthread_cond_wait(&score_ready_cond, &score_mutex); //Aspetto che lo scorer sblocchi il lock per inviare il messaggio finale

            if(gameInfo->currentSession->scores->winner != NULL) sendTextMessage(player->socket_fd, MSG_VINCITORE, gameInfo->currentSession->scores->winner); //Invio il messaggio con il nome del vincitore
            sendTextMessage(player->socket_fd, MSG_PUNTI_FINALI, gameInfo->currentSession->scores->textCSV); //Invio il messaggio di punti finale
            sendNumMessage(player->socket_fd, MSG_TEMPO_ATTESA, gameInfo->currentSession->timeToNextPhase); //Invio il tempo di attesa rimasto

            pthread_mutex_unlock(&score_mutex); //Sblocco il lock dello score
        }
        else sendCurrentGameInfo(gameInfo, player); //Se il gioco sta ricominciando mando la matrice
    }
    pthread_exit(NULL);
}


void* playerSync(void* playerArg) //Funzione del thread per leggere i messaggi dai client
{
    Player* player = (Player*) playerArg; //Casting del parametro a Player*
    if(!addPlayerToLobby(gameInfo, player)) //Provo ad aggiungere il player alla lobby
    {
        sendTextMessage(player->socket_fd, MSG_ERR, "La lobby è piena, disconnessione in corso..."); //Provo ad avvisare il client se non ho già chiuso il socket
        pthread_exit(NULL); //Esco dal thread
    }

    //Game Loop
    while(true)
    {
        Message* msg = readMessage(player->socket_fd); //Rimango in attesa di messaggi da parte del client

        if(msg == NULL) //Errore di lettura, probabile crash del client quindi chiudo il thread correttamente rimuovendo il giocatore dal game
        {
            deleteMessage(msg); //Elimino il messaggio
            pthread_exit(NULL); //Chiudo il thread
        }

        if(gameInfo->currentSession->gamePhase == Off) //Nel caso in cui arrivi un messaggio mentre il server sta chiudendo (molto improbabile)
        {
            sendTextMessage(player->socket_fd, MSG_ERR, "Il gioco sta terminando"); //Provo ad avvisare il client se non ho già chiuso il socket
            deleteMessage(msg); //Elimino il messaggio
            continue;
        }

        switch(msg->type)
        {
            case MSG_REGISTRA_UTENTE:
            {
                if(player->bRegistered) sendTextMessage(player->socket_fd, MSG_ERR, "Sei già stato registrato nella partita"); //Utente già registrato, invio un errore al client
                else{
                    int nameLen = (int)strlen(msg->data);
                    bool usernameValid = nameLen <= 10; //Controllo la lunghezza del nome
                    //Controllo validità caratteri
                    for(int i=0; i<nameLen && usernameValid; i++)
                    {
                        if(!isalnum(msg->data[i])) //Se non è un carattere alfanumerico non è valido
                        {
                            usernameValid = false;
                            break;
                        }
                        msg->data[i] = (char)tolower(msg->data[i]);
                    }
                    if(!usernameValid)
                    {
                        sendTextMessage(player->socket_fd, MSG_ERR, "Nome non valido, usa massimo 10 caratteri alfanumerici"); //Invio un messaggio di errore al client
                        break;
                    }
                    //Richiesta di registrazione
                    for (int i = 0; i < MAX_CLIENTS; i++) {
                        if (gameInfo->currentSession->players[i] != NULL) {
                            if (strcmp(gameInfo->currentSession->players[i]->name, msg->data) == 0) { //Se trovo un utente con lo stesso nome interrompo
                                usernameValid = false;
                                break;
                            }
                        }
                    }
                    if(!usernameValid)
                    {
                        sendTextMessage(player->socket_fd, MSG_ERR, "Nome già presente, riprova con uno differente"); //Invio un messaggio di errore al client
                        break;
                    }
                    //Se i controlli sono stati passati registro
                    copyString(&player->name, msg->data); //Copiando il nome inviato nel messaggio nella struct Player
                    //Aggiungo il player al game
                    if(!addPlayerToGame(gameInfo, player)) //Ritorna false se gli slot sono tutti occupati
                    {
                        sendTextMessage(player->socket_fd, MSG_ERR, "Non ci sono altri slot disponibili, riprova più tardi"); //Invio un messaggio di errore al client
                        break;
                    }
                    player->bRegistered = true; //Setto la flag di controllo
                    sendTextMessage(player->socket_fd, MSG_OK, "Utente registrato correttamente!"); //Invio un messaggio di conferma
                    sendCurrentGameInfo(gameInfo, player); //Invio la matrice e/o il tempo di gioco/attesa
                }
                break;
            }
            case MSG_PAROLA:
            {
                if(!player->bRegistered) sendTextMessage(player->socket_fd, MSG_ERR, "Devi registrarti per usare questo comando");
                else{
                    if(gameInfo->currentSession->gamePhase == Paused) sendTextMessage(player->socket_fd, MSG_ERR, "Il gioco è attualmente in pausa");
                    else
                    {
                        char* word = msg->data;
                        for(int i=0; i<strlen(msg->data); i++) word[i] = (char)tolower(word[i]); //Porto il testo in minuscolo per comodità

                        bool wordFound = findInMatrix(gameInfo->currentSession->currentMatrix, word); //Controllo se esiste la parola nella matrice
                        if(!wordFound) {
                            sendTextMessage(player->socket_fd, MSG_ERR, "Parola non presente nella matrice");
                            break;
                        }
                        bool wordExists = findInTrie(gameInfo->dictionary, word, 0); //Controllo se esista la parola nel dizionario
                        if(!wordExists) {
                            sendTextMessage(player->socket_fd, MSG_ERR, "Parola non valida");
                            break;
                        }
                        bool wordAlreadyFound = findStringInArray(player->foundWords, word); //Controllo se il player ha già trovato quella parola
                        if(wordAlreadyFound) sendNumMessage(player->socket_fd, MSG_PUNTI_PAROLA, 0);
                        else{
                            addStringToArray(player->foundWords, word); //Aggiungo la parola a quelle trovate
                            long score = getWordScore(msg->data); //Prendo e invio lo score della parola al client
                            sendNumMessage(player->socket_fd, MSG_PUNTI_PAROLA, score);
                            player->score += (int)score; //Aggiorno lo score del player
                        }
                    }
                }
                break;
            }
            case MSG_PUNTI_FINALI:
                if(!player->bRegistered) sendTextMessage(player->socket_fd, MSG_ERR, "Devi registrarti per usare questo comando");
                else if(gameInfo->currentSession->gamePhase == Paused)
                {
                    sendTextMessage(player->socket_fd, MSG_PUNTI_FINALI, gameInfo->currentSession->scores->textCSV); //Invio la scoreboard attuale
                    sendCurrentGameInfo(gameInfo, player); //Invio anche le informazioni sul prossimo game
                }
                else if(gameInfo->currentSession->gamePhase == Playing)
                    sendNumMessage(player->socket_fd, MSG_PUNTI_PERSONALI, player->score); //Invio la scoreboard attuale
                else sendTextMessage(player->socket_fd, MSG_ERR, "Non è presente una classifica generale");
                break;
            case MSG_MATRICE:
                if(!player->bRegistered) sendTextMessage(player->socket_fd, MSG_ERR, "Devi registrarti per usare questo comando");
                else sendCurrentGameInfo(gameInfo, player); //Se viene richiesta un matrice mando la matrice e/o il tempo di gioco/attesa
                break;
            case MSG_CLOSE_CLIENT:
                deleteMessage(msg); //Elimino il messaggio
                pthread_exit(NULL); //Esco dal thread
            default:
                sendTextMessage(player->socket_fd, MSG_ERR, "Comando non valido"); //Se il messaggio non è valido invio un messaggio di errore
                break;
        }
        deleteMessage(msg); //Libero la memoria occupata dal messaggio
    }
}


void getOptionalParameters(int argc, char** argv) //Funzione per leggere i parametri opzionali
{
    int option; opterr = 0;
    while ((option = getopt_long_only(argc, argv, "m:t:s:d:", long_options, NULL)) != -1) //Uso gli opt_long per prendere i 4 possibili parametri opzionali m t s d (m:t:s:d: indica che hanno un valore dopo)
    {
        switch (option)
        {
            case 'm':
                if(gameInfo->customMatrixType == Random) exitBeforeStart("Seed e File matrice non possono essere presenti contemporaneamente"); //Se Random era settato ho già un'opzione e interrompo
                if(!updateMatrixFile(gameInfo, optarg)) exitBeforeStart("File matrice non esistente o non corretto"); //Se il file non esiste interrompo, altrimenti lo salvo nella struct
                break;
            case 't':
            {
                int duration;
                if(!strToInt(optarg, &duration) || duration <= 0) exitBeforeStart("Durata di gioco non valido"); //Se il valore della durata è un intero lo converto e lo salvo nella struct, altrimenti interrompo
                gameInfo->gameDuration = duration * 60;
                break;
            }
            case 's':
                if(gameInfo->customMatrixType == File)
                    exitBeforeStart("Seed e File matrice non possono essere presenti contemporaneamente"); //Se File era settato ho già un'opzione e interrompo
                if(!strToInt(optarg, &gameInfo->seed)) exitBeforeStart("Seed non valido"); //Se il valore del seed è un intero lo converto e lo salvo nella struct, altrimenti interrompo
                gameInfo->customMatrixType = Random; //Aggiorno la flag per sapere in quale caso mi trovo
                break;
            case 'd':
                if(!updateDictionaryFile(gameInfo, optarg))
                    exitBeforeStart("File dizionario non esistente o non corretto"); //Se il file non esiste interrompo, altrimenti lo salvo nella struct
                break;
            default:
                exitBeforeStart("Parametri aggiuntivi non riconosciuti"); //Se trovo altri parametri interrompo
        }
    }
    if (optind != argc-2) exitBeforeStart("Parametri aggiuntivi non riconosciuti"); //Se trovo altri parametri interrompo
}


void exitBeforeStart(char* message) //Funzione per uscire liberando la memoria prima dell'inizio del game
{
    freeGameMem(gameInfo); //Libero la memoria occupata dalle strutture di gioco
    exitWithMessage(message); //Esco con un messaggio di errore
}


void exitServerThreadWithMessage(char* message) //Funzione per terminare il thread del server inducendo il processo di terminazione previsto del programma
{
    printf("%s\n", message); //Invio il messaggio di errore
    fflush(stdout);
    kill(getpid(), SIGINT); //Lascio gestire la chiusura all'intercept personalizzato del SIGINT
    pthread_exit(NULL); //Esco dal thread
}