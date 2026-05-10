#ifndef PROJETO_CONTROLLER_H
#define PROJETO_CONTROLLER_H

#include <sys/types.h>
#include <sys/time.h>
#include <glib.h>

// Estrutura com a Informação Interna do Comando
typedef struct comando_info {
    int command_id;
    int user_id;
    pid_t runner_pid;
    pid_t monitor_pid;
    int fd_pai_filho; // Pipe para o pai comunicar com o monitor
    int estado; // 0 = Por iniciar, 1 = Em Execucao, 2 = Terminado
    struct timeval tempo_pedido;
} ComandoInfo;

// Estrutura com o Estado Global do Controller
typedef struct controller_state {
    int parallel_commands;
    int sched_policy; // 0 = FCFS, 1 = Round Robin por utilizador
    int comandos_em_execucao;
    GHashTable *tabela_comandos;
    GQueue *fila_espera;
    GHashTable *filas_por_utilizador; // Para a política Round Robin
    GList *utilizadores_ativos;       // Lista circular de utilizadores para RR
    int is_shutting_down;
    pid_t runner_pid_shutdown;
} ControllerState;

#endif //PROJETO_CONTROLLER_H