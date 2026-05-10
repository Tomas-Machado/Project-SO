#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <sys/time.h>
#include <string.h>
#include <sys/wait.h>

#include "common.h"
#include "controller.h"

// Escreve mensagens em descritores de ficheiro
void print_msg(int fd, const char *msg) {
    write(fd, msg, strlen(msg));
}

// O processo FILHO do Controller atua como um monitor para cada comando
void iniciar_filho_controller(ComandoInfo *cmd) {
    int p[2];
    if (pipe(p) < 0) {
        perror("pipe pai-filho");
        return;
    }

    pid_t pid = fork();

    if (pid == 0) { // No processo filho (monitor)
        close(p[1]); // Fecha escrita no filho
        char fifo_privado[50];
        sprintf(fifo_privado, "/tmp/runner_%d", cmd->runner_pid);

        // Espera pelo OK do Pai (através do pipe) antes de autorizar o Runner
        int ok_pai = 0;
        if (read(p[0], &ok_pai, sizeof(int)) <= 0 || ok_pai != 1) {
            close(p[0]);
            exit(1);
        }
        close(p[0]);

        // Abre o FIFO privado do runner para enviar a autorização de execução
        int fd_resp = open(fifo_privado, O_WRONLY);
        if (fd_resp != -1) {
            int autorizacao = 1;
            write(fd_resp, &autorizacao, sizeof(int));
            close(fd_resp);
        }

        // Abre o mesmo FIFO para leitura para escutar atualizações do runner (início/fim do comando)
        int fd_escuta = open(fifo_privado, O_RDONLY);
        if (fd_escuta == -1) exit(1);

        Operacoes msg_estado;
        ssize_t n;

        // Garante a leitura e o reencaminhamento seguro e completo das mensagens de estado enviadas pelo Runner
        while ((n = read(fd_escuta, &msg_estado, sizeof(Operacoes))) > 0) {
            if (n < (ssize_t)sizeof(Operacoes)) {
                ssize_t total_lido = n;
                while (total_lido < (ssize_t)sizeof(Operacoes)) {
                    ssize_t r = read(fd_escuta, ((char*)&msg_estado) + total_lido, sizeof(Operacoes) - total_lido);
                    if (r <= 0) break;
                    total_lido += r;
                }
                if (total_lido < (ssize_t)sizeof(Operacoes)) continue;
            }
            // Reencaminha a atualização do runner para o FIFO central (SERVER_FIFO) do Controller
            int fd_server = open(SERVER_FIFO, O_WRONLY);
            if (fd_server != -1) {
                write(fd_server, &msg_estado, sizeof(Operacoes));
                close(fd_server);
            }

            // Se o comando terminou (tipo_operacao 4), o monitor encerra a execução
            if (msg_estado.tipo_operacao == 4) break;
        }
        close(fd_escuta);
        
        // O filho termina aqui
        exit(0);
    } else if (pid > 0) {
        close(p[0]); // Fecha leitura no pai
        cmd->monitor_pid = pid;
        cmd->fd_pai_filho = p[1]; // Pai guarda o descritor de escrita para enviar o OK
    }
}

// Calcula a duração da execução e guarda os dados no ficheiro registo.csv
void registar_no_csv(ComandoInfo *cmd) {
    struct timeval tempo_fim;
    gettimeofday(&tempo_fim, NULL);

    // Diferença de tempo entre o pedido e o fim da execução em milissegundos
    long s = tempo_fim.tv_sec - cmd->tempo_pedido.tv_sec;
    long us = tempo_fim.tv_usec - cmd->tempo_pedido.tv_usec;
    double duracao_ms = (s * 1000.0) + (us / 1000.0);

    char buf_csv[256];
    int tam_csv = sprintf(buf_csv, "%d,%d,%.2f\n", cmd->user_id, cmd->command_id, duracao_ms);

    int fd_registo = open("registo.csv", O_WRONLY | O_CREAT | O_APPEND, 0666);
    off_t tam = lseek(fd_registo, 0, SEEK_END);
    
    // Se o ficheiro estiver vazio, escreve o cabeçalho das colunas
    if (tam == 0) {
        char cabecalho[] = "User_ID,Command_ID,Duracao_ms\n";
        write(fd_registo, cabecalho, sizeof(cabecalho) - 1);
    }
    write(fd_registo, buf_csv, tam_csv);
    close(fd_registo);
}

// Função auxiliar para verificar se todas as filas estão vazias
int todas_as_filas_vazias(ControllerState *state) {
    if (state->sched_policy == 0) {
        return g_queue_is_empty(state->fila_espera);
    } else {
        GList *list = g_hash_table_get_values(state->filas_por_utilizador);
        for (GList *l = list; l != NULL; l = l->next) {
            if (!g_queue_is_empty((GQueue*)l->data)) {
                g_list_free(list);
                return 0;
            }
        }
        g_list_free(list);
        return 1;
    }
}

// Seleciona o próximo comando a ser executado da fila de espera,
// respeitando a política de escalonamento configurada (FCFS ou Round Robin),
// e inicia-o se houver "slots" de execução livres.
void promover_proximo(ControllerState *state) {
    ComandoInfo *cmd_pendente = NULL;

    if (state->sched_policy == 0) { // Política FCFS (First-Come, First-Served)
        if (!g_queue_is_empty(state->fila_espera)) {
            cmd_pendente = g_queue_pop_head(state->fila_espera);
        }
    } else { // Política Round Robin por utilizador
        if (state->utilizadores_ativos != NULL) {
            GList *atual = state->utilizadores_ativos;
            int user_id = GPOINTER_TO_INT(atual->data);
            GQueue *fila_user = g_hash_table_lookup(state->filas_por_utilizador, GINT_TO_POINTER(user_id));

            if (fila_user && !g_queue_is_empty(fila_user)) {
                cmd_pendente = g_queue_pop_head(fila_user);
            }

            // Move o ponteiro para o próximo utilizador na lista circular
            GList *proximo = g_list_next(atual);
            if (proximo == NULL) {
                proximo = g_list_first(state->utilizadores_ativos);
            }

            // Se o utilizador atual já não tem comandos, remove-o da lista de ativos
            if (fila_user && g_queue_is_empty(fila_user)) {
                if (proximo == atual) {
                    state->utilizadores_ativos = NULL;
                } else {
                    state->utilizadores_ativos = proximo;
                }
                state->utilizadores_ativos = g_list_remove_link(state->utilizadores_ativos, atual);
                g_list_free_1(atual);
            } else {
                state->utilizadores_ativos = proximo;
            }
        }
    }

    if (cmd_pendente) {
        state->comandos_em_execucao++;
        printf("[Controller] Comando %d saiu da fila! A autorizar monitor %d\n", cmd_pendente->command_id, cmd_pendente->monitor_pid);
        
        int ok = 1;
        write(cmd_pendente->fd_pai_filho, &ok, sizeof(int));
        close(cmd_pendente->fd_pai_filho); // Já não precisa de enviar mais nada
        cmd_pendente->fd_pai_filho = -1;
    }
}

// Recebe um novo comando de um runner. Se houver capacidade, inicia-o logo;
// caso contrário, coloca-o na fila de espera apropriada.
void handle_novo_pedido(Operacoes *pedido, ControllerState *state) {
    if (state->is_shutting_down == 1) {
        // Apenas printf para debug no servidor, conforme permitido.
        printf("[Controller] Pedido rejeitado: o servidor esta a encerrar.\n");
        return;
    }

    ComandoInfo *novo_cmd = g_new(ComandoInfo, 1);
    novo_cmd->command_id = pedido->command_id;
    novo_cmd->user_id = pedido->user_id;
    novo_cmd->runner_pid = pedido->pid;
    gettimeofday(&novo_cmd->tempo_pedido, NULL); // Tempo de receção do pedido
    novo_cmd->estado = 0;

    g_hash_table_insert(state->tabela_comandos, GINT_TO_POINTER(novo_cmd->command_id), novo_cmd);

    if (state->comandos_em_execucao < state->parallel_commands) {
        state->comandos_em_execucao++;
        iniciar_filho_controller(novo_cmd);
        
        // Como há vaga imediata, envia logo o OK para o monitor
        int ok = 1;
        write(novo_cmd->fd_pai_filho, &ok, sizeof(int));
        close(novo_cmd->fd_pai_filho);
        novo_cmd->fd_pai_filho = -1;
    } else {
        printf("[Controller] Limite! Comando %d na fila.\n", novo_cmd->command_id);
        
        // Cria o monitor mesmo estando em fila
        iniciar_filho_controller(novo_cmd);

        if (state->sched_policy == 0) {
            g_queue_push_tail(state->fila_espera, novo_cmd);
        } else {
            GQueue *fila_user = g_hash_table_lookup(state->filas_por_utilizador, GINT_TO_POINTER(novo_cmd->user_id));
            if (!fila_user) {
                fila_user = g_queue_new();
                g_hash_table_insert(state->filas_por_utilizador, GINT_TO_POINTER(novo_cmd->user_id), fila_user);
                state->utilizadores_ativos = g_list_append(state->utilizadores_ativos, GINT_TO_POINTER(novo_cmd->user_id));
            }
            g_queue_push_tail(fila_user, novo_cmd);
        }
    }
}

// Atualiza o estado de um comando para "Em Execução" e regista o timestamp de início.
void handle_inicio_execucao(Operacoes *pedido, ControllerState *state) {
    ComandoInfo *cmd = g_hash_table_lookup(state->tabela_comandos, GINT_TO_POINTER(pedido->command_id));

    if (cmd != NULL) {
        cmd->estado = 1;

        printf("[Controller] Comando %d (Estado: Em Execucao)\n", cmd->command_id);
    }
}

// Chamada quando um comando termina. Regista no CSV,
// liberta o "slot" de execução e chama promover_proximo para ocupar o lugar vago.
void handle_fim_execucao(Operacoes *pedido, ControllerState *state) {
    ComandoInfo *cmd = g_hash_table_lookup(state->tabela_comandos, GINT_TO_POINTER(pedido->command_id));

    if (cmd != NULL) {
        cmd->estado = 2;
        registar_no_csv(cmd);

        printf("[Controller] Comando %d (Estado: Terminado).\n", cmd->command_id);

        // Aguarda o término do processo monitor para evitar zombies
        waitpid(cmd->monitor_pid, NULL, 0);

        g_hash_table_remove(state->tabela_comandos, GINT_TO_POINTER(cmd->command_id));
        state->comandos_em_execucao--;

        promover_proximo(state);
    }

    // Verificação robusta de shutdown
    if (state->is_shutting_down == 1 && state->comandos_em_execucao == 0 && todas_as_filas_vazias(state)) {
        char fifo[50];
        sprintf(fifo, "/tmp/runner_%d", state->runner_pid_shutdown);
        int fd = open(fifo, O_WRONLY);
        if (fd != -1) {
            int ok = 1;
            write(fd, &ok, sizeof(int));
            close(fd);
        }
        unlink(SERVER_FIFO);
        printf("[Controller] Todas as tarefas pendentes terminaram. A desligar...\n");
        exit(0);
    }
}

// Constrói uma string com todos os comandos que estão atualmente na tabela de comandos
// (em execução) ou nas filas de espera, e envia-a de volta ao runner que solicitou o status.
void handle_consulta_estado(Operacoes *pedido, ControllerState *state) {
    char fifo_privado[50];
    sprintf(fifo_privado, "/tmp/runner_%d", pedido->pid);

    int fd_resp = open(fifo_privado, O_WRONLY);
    if (fd_resp == -1) {
        print_msg(STDERR_FILENO, "Erro ao abrir o FIFO do Runner para consulta.\n");
        return;
    }

    char buffer[256];
    int len;

    // COMANDOS EM EXECUÇÃO
    print_msg(fd_resp, "Executing\n");

    GHashTableIter iter;
    gpointer key, value;
    g_hash_table_iter_init(&iter, state->tabela_comandos);

    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ComandoInfo *cmd = (ComandoInfo *)value;
        if (cmd->estado == 1) {
            len = sprintf(buffer, "user-id %d - command-id %d\n", cmd->user_id, cmd->command_id);
            write(fd_resp, buffer, len);
        }
    }

    // COMANDOS AGENDADOS
    print_msg(fd_resp, "Scheduled\n");

    g_hash_table_iter_init(&iter, state->tabela_comandos);
    while (g_hash_table_iter_next(&iter, &key, &value)) {
        ComandoInfo *cmd = (ComandoInfo *)value;

        int em_espera = 0;
        if (state->sched_policy == 0) {
            em_espera = (g_queue_find(state->fila_espera, cmd) != NULL);
        } else {
            GList *list = g_hash_table_get_values(state->filas_por_utilizador);
            for (GList *l = list; l != NULL; l = l->next) {
                if (g_queue_find((GQueue*)l->data, cmd)) {
                    em_espera = 1;
                    break;
                }
            }
            g_list_free(list);
        }

        if (cmd->estado == 0 && !em_espera) {
            len = sprintf(buffer, "user-id %d - command-id %d\n", cmd->user_id, cmd->command_id);
            write(fd_resp, buffer, len);
        }
    }

    if (state->sched_policy == 0) {
        for (GList *l = state->fila_espera->head; l != NULL; l = l->next) {
            ComandoInfo *cmd = (ComandoInfo *)l->data;
            len = sprintf(buffer, "user-id %d - command-id %d\n", cmd->user_id, cmd->command_id);
            write(fd_resp, buffer, len);
        }
    } else {
        GList *users = g_hash_table_get_keys(state->filas_por_utilizador);
        for (GList *u = users; u != NULL; u = u->next) {
            GQueue *q = g_hash_table_lookup(state->filas_por_utilizador, u->data);
            for (GList *l = q->head; l != NULL; l = l->next) {
                ComandoInfo *cmd = (ComandoInfo *)l->data;
                len = sprintf(buffer, "user-id %d - command-id %d\n", cmd->user_id, cmd->command_id);
                write(fd_resp, buffer, len);
            }
        }
        g_list_free(users);
    }

    close(fd_resp);
}

// Ativa o modo de encerramento, impedindo novos pedidos e preparando
// o controlador para fechar assim que os comandos atuais e em espera terminarem.
void handle_shutdown(Operacoes *pedido, ControllerState *state) {
    printf("[Controller] Recebeu pedido de shutdown (-s). A aguardar término de tarefas...\n");

    state->is_shutting_down = 1;
    state->runner_pid_shutdown = pedido->pid;

    // Se já não houver tarefas em execução nem em fila, encerra imediatamente
    if (state->is_shutting_down == 1 && state->comandos_em_execucao == 0 && todas_as_filas_vazias(state)) {
        char fifo[50];
        sprintf(fifo, "/tmp/runner_%d", state->runner_pid_shutdown);
        int fd = open(fifo, O_WRONLY);
        if (fd != -1) {
            int ok = 1;
            write(fd, &ok, sizeof(int));
            close(fd);
        }
        unlink(SERVER_FIFO);
        printf("[Controller] Todas as tarefas terminadas. A desligar...\n");
        exit(0);
    }
}

int main (int argc, char *argv[])
{
    if (argc != 3) {
        print_msg(STDERR_FILENO, "Uso: ./controller <parallel-commands> <sched-policy>\n");
        return 1;
    }

    ControllerState estado_servidor;
    estado_servidor.parallel_commands = atoi(argv[1]);
    estado_servidor.sched_policy = atoi(argv[2]);
    estado_servidor.comandos_em_execucao = 0;
    estado_servidor.tabela_comandos = g_hash_table_new(g_direct_hash, g_direct_equal);
    estado_servidor.fila_espera = g_queue_new();
    estado_servidor.filas_por_utilizador = g_hash_table_new(g_direct_hash, g_direct_equal);
    estado_servidor.utilizadores_ativos = NULL;
    estado_servidor.is_shutting_down = 0;

    unlink(SERVER_FIFO);
    if (mkfifo(SERVER_FIFO, 0666) == -1) {
        print_msg(STDERR_FILENO, "Erro ao criar o FIFO do servidor.\n");
        return 1;
    }

    int server_fd = open(SERVER_FIFO, O_RDWR);

    printf("[Controller] A escuta. Concorrencia: %d\n", estado_servidor.parallel_commands);

    Operacoes pedido;
    ssize_t bytes_lidos;

    // Loop principal de leitura do FIFO central: o servidor bloqueia aqui à espera de novas mensagens
    while (1)
    {
        bytes_lidos = read(server_fd, &pedido, sizeof(Operacoes));
        if (bytes_lidos <= 0) break;

        if (bytes_lidos < (ssize_t)sizeof(Operacoes)) {
            // Se leu menos do que uma estrutura completa, tenta ler o resto
            ssize_t total_lido = bytes_lidos;
            while (total_lido < (ssize_t)sizeof(Operacoes)) {
                ssize_t n = read(server_fd, ((char*)&pedido) + total_lido, sizeof(Operacoes) - total_lido);
                if (n <= 0) break;
                total_lido += n;
            }
            if (total_lido < (ssize_t)sizeof(Operacoes)) continue;
        }

        switch (pedido.tipo_operacao) {
                case 1: // Novo pedido de execução de comando
                    handle_novo_pedido(&pedido, &estado_servidor);
                    break;
                case 2: // Pedido de consulta de estado (status)
                    handle_consulta_estado(&pedido, &estado_servidor);
                    break;
                case 3: // Pedido de encerramento do servidor (shutdown)
                    handle_shutdown(&pedido, &estado_servidor);
                    break;
                case 4: // Notificação de fim de execução vinda de um monitor
                    handle_fim_execucao(&pedido, &estado_servidor);
                    break;
                case 5: // Notificação de início de execução vinda de um monitor
                    handle_inicio_execucao(&pedido, &estado_servidor);
                    break;
                default:
                    // Se o tipo de operação for inválido, pode ser lixo no FIFO ou erro de sincronização
                    // printf("[Controller] Operação desconhecida (%d) recebida. Ignorando...\n", pedido.tipo_operacao);
                    break;
            }
        }
    return 0;
}