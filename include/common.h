#ifndef PROJETO_COMMON_H
#define PROJETO_COMMON_H

#include <sys/types.h>
#include <sys/time.h>
#include <glib.h>

#define SERVER_FIFO "/tmp/server_fifo_projeto" // Caminho do FIFO principal do controller

#define MAX_ARGS 4096 // Tamanho maximo que os argumentos podem ter

// Estrutura do Pedido (usada para comunicar entre Runner e Controller)
typedef struct operacoes
{
    pid_t pid;         // Controller saber o nome do FIFO da resposta
    int tipo_operacao; // 1 -> '-e', 2 -> '-c', 3 -> '-s', 4 -> 'Terminado', 5 -> 'A iniciar'
    int user_id;       // Identificador do utilizador
    int command_id;    // Identificador unico do comando
    char comando[MAX_ARGS]; // Guardar comando e argumentos
} Operacoes;

#endif //PROJETO_COMMON_H