#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include "sqlite3.h"
#include <arpa/inet.h>
#include <queue>
#include <iostream>
#include <sys/select.h>

#define PORT 2908
#define BUFFER_SIZE 1024
#define GAME_TIMER 5
#define QUESTION_TIMER 7

sqlite3 *db;
pthread_mutex_t db_mutex;
pthread_mutex_t mutex;

pthread_mutex_t winner_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t winner_cond = PTHREAD_COND_INITIALIZER;
pthread_mutex_t send_winner_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t _send_winner_cond = PTHREAD_COND_INITIALIZER;
int winner_announced_clients = 0;

pthread_mutex_t ready_clients_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t ready_clients_cond = PTHREAD_COND_INITIALIZER;
int ready_clients = 0; 

pthread_cond_t game_end_cond = PTHREAD_COND_INITIALIZER;
int waiting_clients = 0;

pthread_mutex_t finish_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t finish_cond = PTHREAD_COND_INITIALIZER;
int finished_clients = 0;

int countdown_time = GAME_TIMER;
pthread_mutex_t countdown_mutex = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t countdown_cond = PTHREAD_COND_INITIALIZER;
bool countdown_complete = false;

int total_clients;
char *buffer;
bool game_is_ongoing = false;
int max_score = -1;

struct Client
{
  int socket;
  int score;
  pthread_mutex_t mutex;
  int id;
  int question_id;
  char correct_answer;
  char name[256];
};

int winner_socket = -1;
int winner_score = -1;
int winner_id = INT16_MAX;
char winner_name[256] = {};

std::queue<Client *> client_queue;

void initialize_database()
{

  char *err_msg = NULL;

  if (sqlite3_open("quiz_questions.db", &db) != SQLITE_OK)
  {
    fprintf(stderr, "Failed to open database:%s\n", sqlite3_errmsg(db));
    exit(EXIT_FAILURE);
  }

  const char *create_sql_table = "CREATE TABLE IF NOT EXISTS questions("
                                 "id INTEGER PRIMARY KEY AUTOINCREMENT,"
                                 "question TEXT,"
                                 "option_a TEXT,"
                                 "option_b TEXT,"
                                 "option_c TEXT,"
                                 "option_d TEXT,"
                                 "correct_option TEXT,"
                                 "UNIQUE(question));";

  if (sqlite3_exec(db, create_sql_table, 0, 0, &err_msg) != SQLITE_OK)
  {
    fprintf(stderr, "SQL create error: %s\n", err_msg);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    exit(EXIT_FAILURE);
  }

  const char *insert_questions_sql =
      "INSERT OR IGNORE INTO questions (question, option_a, option_b, option_c, option_d, correct_option) "
      "VALUES "
      "('What is the capital of France?', 'Paris', 'Berlin', 'Madrid', 'Rome', 'A' ),"
      "('What is the capital city of New Zealand?', 'Auckland', 'Christchurch', 'Wellington', 'Queenstown', 'C' ),"
      "('What is the closest planet to the Sun?', 'Venus', 'Mercury', 'Mars', 'Jupiter', 'B' ),"
      "('What is 5!(factorial)?', '100', '60', '225', '120', 'D' );";

  if (sqlite3_exec(db, insert_questions_sql, 0, 0, &err_msg) != SQLITE_OK)
  {
    fprintf(stderr, "SQL insert error: %s\n", err_msg);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    exit(EXIT_FAILURE);
  }
}

static int questions_count_callback(void *count_ptr, int argc, char **argv, char **azColName)
{
  *(int *)count_ptr = atoi(argv[0]);
  return 0;
}

int get_question_count()
{
  int question_count = 0;
  char *err_msg = NULL;

  const char *count_query = "SELECT COUNT(*) FROM questions;";

  pthread_mutex_lock(&db_mutex);

  if (sqlite3_exec(db, count_query, questions_count_callback, &question_count, &err_msg) != SQLITE_OK)
  {
    fprintf(stderr, "SQL query error: %s\n", err_msg);
    sqlite3_free(err_msg);
    pthread_mutex_unlock(&db_mutex);
    return -1;
  }

  pthread_mutex_unlock(&db_mutex);

  printf("Question count in function: %d\n", question_count);
  return question_count;
}

static int send_question_callback(void *data, int argc, char **argv, char **azColName)
{
  Client *client = (Client *)data;
  char local_buffer[BUFFER_SIZE];
  memset(local_buffer, 0, sizeof(local_buffer));
  if (argc == 7)
  {

    snprintf(local_buffer, BUFFER_SIZE, "%s.%s\nA) %s\nB) %s\nC) %s\nD) %s\n\n", argv[6], argv[0], argv[1], argv[2], argv[3], argv[4]);
    strcat(local_buffer, "Your answer (A/B/C/D): ");
    printf("\nIntrebarea este: %s\n", local_buffer);
    write(client->socket, &local_buffer, strlen(local_buffer));
  }

  client->correct_answer = argv[5][0];

  return 0;
}

void get_questions(Client *client)
{

  pthread_mutex_lock(&db_mutex);

  char query[256];
  snprintf(query, sizeof(query), "SELECT question, option_a, option_b, option_c, option_d, correct_option, id FROM questions "
                                 "WHERE id = %d;",
           client->question_id);

  char *err_msg = NULL;
  if (sqlite3_exec(db, query, send_question_callback, client, &err_msg) != SQLITE_OK)
  {
    fprintf(stderr, "SQL query error: %s\n", err_msg);
    sqlite3_free(err_msg);
    sqlite3_close(db);
    exit(EXIT_FAILURE);
  }

  pthread_mutex_unlock(&db_mutex);
}

void reset_game_state()
{
  pthread_mutex_lock(&winner_mutex);
  winner_score = -1;
  winner_id = INT16_MAX;
  memset(winner_name, 0, sizeof(winner_name));
  winner_announced_clients = 0;
  pthread_mutex_unlock(&winner_mutex);

  pthread_mutex_lock(&ready_clients_mutex);
  ready_clients = 0;
  pthread_mutex_unlock(&ready_clients_mutex);

  pthread_mutex_lock(&finish_mutex);
  finished_clients = 0;
  pthread_mutex_unlock(&finish_mutex);

  pthread_mutex_lock(&mutex);
  total_clients = 0;
  client_queue = std::queue<Client *>();
  pthread_mutex_unlock(&mutex);
}

void reset_countdown()
{
  pthread_mutex_lock(&countdown_mutex);
  countdown_time = GAME_TIMER;
  countdown_complete = false;
  pthread_mutex_unlock(&countdown_mutex);
}

void *handle_client(void *arg)
{
  printf("intrat in functia handle_client\n");

  Client *client = (Client *)arg;
  client->question_id = 1;
  client->score = 0;
  char client_answer;
  int total_questions = get_question_count();
  char buf[1024];

  write(client->socket, "Enter your name: ", 18);
  read(client->socket, client->name, sizeof(client->name));
  client->name[strcspn(client->name, "\n")] = '\0'; //

  printf("Client connected: ID = %d, Name = %s\n", client->id, client->name);

  

  pthread_mutex_lock(&ready_clients_mutex);
  ready_clients++;
  if (ready_clients >= 2)
  {
    pthread_cond_broadcast(&ready_clients_cond);
  }
  pthread_mutex_unlock(&ready_clients_mutex);

  pthread_mutex_lock(&ready_clients_mutex);
  while (ready_clients < 2)
  {
    pthread_cond_wait(&ready_clients_cond, &ready_clients_mutex);
  }
  pthread_mutex_unlock(&ready_clients_mutex);

  pthread_mutex_lock(&countdown_mutex);
  while (!countdown_complete)
  {
    pthread_cond_wait(&countdown_cond, &countdown_mutex);
  }
  pthread_mutex_unlock(&countdown_mutex);

 

  while (1)
  {
    get_questions(client);

    fd_set read_fds;
    struct timeval timeout;
    FD_ZERO(&read_fds);
    FD_SET(client->socket, &read_fds);

    timeout.tv_sec = QUESTION_TIMER;
    timeout.tv_usec = 0;

    int activity = select(client->socket + 1, &read_fds, NULL, NULL, &timeout);

    if (activity > 0 && FD_ISSET(client->socket, &read_fds))
    {

      printf("Am primit activitate de la client ID %d\n", client->id);

      recv(client->socket, &client_answer, sizeof(char), 0);

      printf("Clients's answer is: %c\n", client_answer);

      if (client_answer == client->correct_answer)
      {

        client->score++;

        strcpy(buf, "\nYour answer is correct!\n\n");

        write(client->socket, &buf, sizeof(buf));

        printf("Your answer is correct!\n");
      }
      else
      {

        char correct_answer_str[2];
        snprintf(correct_answer_str, sizeof(correct_answer_str), "%c", client->correct_answer);

        strcpy(buf, "\nYour answer is wrong! Correct answer was ");
        strcat(buf, correct_answer_str);
        strcat(buf, ")\n\n");

        write(client->socket, &buf, sizeof(buf));

        printf("Your answer is wrong! Correct answer was %c)\n", client->correct_answer);
      }
    }
    else
    {
      strcpy(buf, "\nTime's up! Moving to the next question.\n\n");
      write(client->socket, buf, strlen(buf));
      printf("Time's up for client ID %d\n", client->id);
    }

    client->question_id++;

    if (client->question_id > total_questions)
    {
      char score_str[10], id_str[10];
      snprintf(score_str, sizeof(score_str), "%d", client->score);
      snprintf(id_str, sizeof(id_str), "%d", client->id);

      strcpy(buf, "\nNo more questions!\nYour score was: ");
      strcat(buf, score_str);
      strcat(buf, "\n");
      strcat(buf, "Your client id was: ");
      strcat(buf, id_str);
      strcat(buf, "\n");

      write(client->socket, &buf, sizeof(buf));

      printf("\nNo more questions!\nYour score was: %d\n", client->score);

      break;
    }
  }

  pthread_mutex_lock(&winner_mutex);
  if (client->score > winner_score || (client->score == winner_score && client->id < winner_id))
  {
    winner_score = client->score;
    winner_id = client->id;
    strncpy(winner_name, client->name, sizeof(winner_name));
    printf("New winner: ID = %d, Name = %s, Score = %d\n", winner_id, winner_name, winner_score);
  }
  pthread_mutex_unlock(&winner_mutex);

  pthread_mutex_lock(&finish_mutex);
  finished_clients++;

  if (finished_clients == ready_clients)
  {
    pthread_cond_broadcast(&finish_cond);
  }
  else
  {

    pthread_cond_wait(&finish_cond, &finish_mutex);
  }
  pthread_mutex_unlock(&finish_mutex);

  sleep(1);

  pthread_mutex_lock(&winner_mutex);
  char winner_msg[BUFFER_SIZE];
  snprintf(winner_msg, sizeof(winner_msg), "Game Over! Winner is: %s (ID: %d) with a score of %d.\n",
           winner_name, winner_id, winner_score);
  write(client->socket, winner_msg, strlen(winner_msg));

  winner_announced_clients++;
  if (winner_announced_clients == ready_clients)
  {
    
    pthread_cond_signal(&winner_cond);
  }

  pthread_mutex_unlock(&winner_mutex);

  printf("iesit din functia handle_client in clientul cu id-ul: %d\n", client->id);
  close(client->socket);
  free(client);

  return NULL;
}

void *start_countdown_timer(void *arg)
{

  while (1)
  {
    pthread_mutex_lock(&ready_clients_mutex);
    while (ready_clients < 2)
    {
      pthread_cond_wait(&ready_clients_cond, &ready_clients_mutex);
    }
    pthread_mutex_unlock(&ready_clients_mutex);

    while (countdown_time > 0)
    {
      std::queue<Client *> temp_queue = client_queue;

      while (!temp_queue.empty())
      {
        Client *client = temp_queue.front();
        temp_queue.pop();
        char buf[1024] = {};
        snprintf(buf, sizeof(buf), "Game starting in %d seconds...\n", countdown_time);
        write(client->socket, buf, strlen(buf));
      }
      countdown_time--;
      sleep(1);
    }

    std::queue<Client *> temp_queue = client_queue;
    while (!temp_queue.empty())
    {
      Client *client = temp_queue.front();
      temp_queue.pop();
      char buf[1024] = {};
      snprintf(buf, sizeof(buf), "Game starting now!\n\n You will have %d seconds to respond to each question!\n\n", QUESTION_TIMER);
      write(client->socket, buf, strlen(buf));
    }

    pthread_mutex_lock(&countdown_mutex);
    countdown_complete = true;
    pthread_cond_broadcast(&countdown_cond);
    pthread_mutex_unlock(&countdown_mutex);

    pthread_mutex_lock(&mutex);
    game_is_ongoing = true;
    pthread_mutex_unlock(&mutex);

    pthread_mutex_lock(&winner_mutex);
    while (winner_announced_clients < ready_clients)
    {
      pthread_cond_wait(&winner_cond, &winner_mutex);
    }
    pthread_mutex_unlock(&winner_mutex);

    reset_game_state();
    reset_countdown();

    pthread_mutex_lock(&ready_clients_mutex);
    game_is_ongoing = false;

   
    if (waiting_clients > 0)
    {
      pthread_cond_broadcast(&game_end_cond);
    }
    pthread_mutex_unlock(&ready_clients_mutex);
  }

  

  return NULL;
}

int main()
{
  struct sockaddr_in server; // structura folosita de server
  struct sockaddr_in from;
  int sd; // descriptorul de socket
  // pthread_t th[100];    //Identificatorii thread-urilor care se vor crea
  pthread_t th, countdown_th;

  initialize_database();

  /* crearea unui socket */
  if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
  {
    perror("[server]Eroare la socket().\n");
    exit(EXIT_FAILURE);
  }
  /* utilizarea optiunii SO_REUSEADDR */
  int on = 1;
  setsockopt(sd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));

  /* pregatirea structurilor de date */
  bzero(&server, sizeof(server));
  bzero(&from, sizeof(from));

  /* umplem structura folosita de server */
  /* stabilirea familiei de socket-uri */
  server.sin_family = AF_INET;
  /* acceptam orice adresa */
  server.sin_addr.s_addr = htonl(INADDR_ANY);
  /* utilizam un port utilizator */
  server.sin_port = htons(PORT);

  /* atasam socketul */
  if (bind(sd, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
  {
    perror("[server]Eroare la bind().\n");
    close(sd);
    exit(EXIT_FAILURE);
  }

  /* punem serverul sa asculte daca vin clienti sa se conecteze */
  if (listen(sd, 2) == -1)
  {
    perror("[server]Eroare la listen().\n");
    close(sd);
    exit(EXIT_FAILURE);
  }
  /* servim in mod concurent clientii...folosind thread-uri */

  pthread_create(&countdown_th, NULL, start_countdown_timer, NULL);
  pthread_detach(countdown_th);

  while (1)
  {
    int client_socket;

    socklen_t length = sizeof(from);

    printf("[server]Asteptam la portul %d...\n", PORT);
    fflush(stdout);

    /* acceptam un client (stare blocanta pina la realizarea conexiunii) */
    if ((client_socket = accept(sd, (struct sockaddr *)&from, &length)) < 0)
    {
      perror("[server]Eroare la accept().\n");
      continue;
    }

    pthread_mutex_lock(&ready_clients_mutex);

    if (game_is_ongoing)
    {
      waiting_clients++;
      pthread_cond_wait(&game_end_cond, &ready_clients_mutex);
      waiting_clients--;
    }
    pthread_mutex_unlock(&ready_clients_mutex);

    Client *client = (Client *)malloc(sizeof(Client));
    client->socket = client_socket;

    total_clients++;

    client->id = total_clients;

    client_queue.push(client);

    pthread_create(&th, NULL, handle_client, (void *)client);
    pthread_detach(th);

  } // while

  close(sd);
  return 0;
}
