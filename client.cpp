#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <arpa/inet.h>
#include <iostream>
#include <sys/select.h>

#define QUESTION_TIMER 7

int port;

int main(int argc, char *argv[])
{
  int sd;                    // descriptorul de socket
  struct sockaddr_in server; // structura folosita pentru conectare
                             // mesajul trimis

  /* exista toate argumentele in linia de comanda? */
  if (argc != 3)
  {
    printf("Sintaxa: %s <adresa_server> <port>\n", argv[0]);
    return -1;
  }

  /* stabilim portul */
  port = atoi(argv[2]);

  /* cream socketul */
  if ((sd = socket(AF_INET, SOCK_STREAM, 0)) == -1)
  {
    perror("Eroare la socket().\n");
    exit(EXIT_FAILURE);
  }

  /* umplem structura folosita pentru realizarea conexiunii cu serverul */
  /* familia socket-ului */
  server.sin_family = AF_INET;
  /* adresa IP a serverului */
  server.sin_addr.s_addr = inet_addr(argv[1]);
  /* portul de conectare */
  server.sin_port = htons(port);

  /* ne conectam la server */
  if (connect(sd, (struct sockaddr *)&server, sizeof(struct sockaddr)) == -1)
  {
    perror("[client]Eroare la connect().\n");
    exit(EXIT_FAILURE);
  }

  
  char name[256];
  char server_message[1024];

  // Primesc mesajul "Enter your name: " de la server
  int bytes_received = read(sd, server_message, sizeof(server_message) - 1);
  if (bytes_received <= 0)
  {
    perror("[client] Serverul s-a deconectat.\n");
    close(sd);
    return -1;
  }
  server_message[bytes_received] = '\0';
  printf("%s", server_message);

  
  fgets(name, sizeof(name), stdin);
  write(sd, name, strlen(name));

  // Afișez numărătoarea inversă primită de la server
  while (true)
  {
    bytes_received = read(sd, server_message, sizeof(server_message) - 1);
    if (bytes_received <= 0)
    {
      perror("[client] Serverul s-a deconectat.\n");
      close(sd);
      return -1;
    }
    server_message[bytes_received] = '\0';
    printf("%s", server_message);

    if (strstr(server_message, "Game starting now!"))
    {
      break;
    }
  }

  sleep(2);

  while (1)
  {
    char question_buffer[1024] = {};


    fd_set read_fds;
    struct timeval timeout;
    FD_ZERO(&read_fds);
    FD_SET(STDIN_FILENO, &read_fds);
    FD_SET(sd, &read_fds);

    timeout.tv_sec = QUESTION_TIMER;
    timeout.tv_usec = 0;

    int max_fd = (STDIN_FILENO > sd) ? STDIN_FILENO : sd;

    int activity = select(max_fd + 1, &read_fds, NULL, NULL, &timeout);

    if (activity > 0 && FD_ISSET(STDIN_FILENO, &read_fds))
    {
      printf("Am primit activitate de la tastatura\n");
      char answer[4];
      scanf("%s", answer);
      printf("Am citit raspunsul: %s\n", answer);
      send(sd, &answer, sizeof(answer[0]), 0);
    }

    if (activity > 0 && FD_ISSET(sd, &read_fds))
    {
        printf("Am primit activitate de la server\n");
      int bytes_received = read(sd, &question_buffer, sizeof(question_buffer));
      if (bytes_received <= 0)
      {
        break;
      }

        int numar;
     
        if (sscanf(question_buffer, "%d.", &numar) == 1)
      {
          printf("Am primit intrebarea cu numarul %n\n",&numar);
        if (strstr(question_buffer, "Game Over!") != NULL)
        {
          printf("%s", question_buffer);
          close(sd);
          break;
        }
      }else if (strcmp(question_buffer, "\nTime's up! Moving to the next question.\n\n") == 0){
          printf("Am primit mesajul Time's up!\n");
          fflush(stdin);
      }else{
          printf("Am primit un raspuns de la server dupa verificarea raspunsului clientului \n");
          printf("Afisez raspunsul primit de la verficarea raspunsului clientului:\n");
      }

      question_buffer[bytes_received] = '\0'; 
      printf("%s\n", question_buffer);  
    }

   
  }
  /* inchidem conexiunea, am terminat */
  
  close(sd);

  return 0;
}