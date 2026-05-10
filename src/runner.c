#include <stdio.h>
#include <stdlib.h>
#include <glib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <ctype.h>
#include <errno.h>
#include "common.h"


// Escrever mensagens num descritor de ficheiro
void print_msg(int fd, const char *msg) {
    write(fd, msg, strlen(msg));
}

// PARSING
char* trim_whitespace(char* str) {
    char* end;
    while(isspace((unsigned char)*str)) str++;
    if(*str == 0) return str;
    end = str + strlen(str) - 1;
    while(end > str && isspace((unsigned char)*end)) end--;
    end[1] = '\0';
    return str;
}


void execute_pipeline(char* command_str) {
    char* commands[10];
    int num_commands = 0;
    
    // Fazer uma cópia da string para o strtok inicial
    char str_copy[MAX_ARGS];
    strncpy(str_copy, command_str, MAX_ARGS - 1);
    str_copy[MAX_ARGS - 1] = '\0';

    // Divide a string do comando em segmentos separados por pipes ('|')
    char* token = strtok(str_copy, "|");
    while (token != NULL && num_commands < 10) {
        commands[num_commands++] = trim_whitespace(token);
        token = strtok(NULL, "|");
    }

    if (num_commands == 0) return;

    int num_pipes = num_commands - 1;
    int pipefds[2 * num_pipes];

    // Cria os pipes necessários: pipefds armazena os descritores de leitura e escrita para cada pipe
    for (int i = 0; i < num_pipes; i++) {
        if (pipe(pipefds + i * 2) < 0) {
            perror("pipe");
            exit(1);
        }
    }

    for (int i = 0; i < num_commands; i++) {
        // Cria os processos filhos para cada comando do pipeline
        pid_t pid = fork();
        if (pid == 0) {
            char** args;
            int arg_count = 0;
            GError *error = NULL;
            
            if (!g_shell_parse_argv(commands[i], &arg_count, &args, &error)) {
                char err_buf[256];
                int err_len = sprintf(err_buf, "Erro ao processar comando: %s\n", error->message);
                write(STDERR_FILENO, err_buf, err_len);
                g_error_free(error);
                exit(1);
            }
            
            char* input_file = NULL;
            char* output_file = NULL;
            int error_redirect = 0;
            char* error_file = NULL;

            // Identificar redirecionamentos manualmente nos argumentos processados pela glib
            int j = 0;
            while (args[j] != NULL) {
                if (strcmp(args[j], ">") == 0) { // Redirecionamento de saída (stdout)
                    output_file = args[j+1];
                    args[j] = NULL;
                    j += 2;
                } else if (strcmp(args[j], "<") == 0) { // Redirecionamento de entrada (stdin)
                    input_file = args[j+1];
                    args[j] = NULL;
                    j += 2;
                } else if (strcmp(args[j], "2>") == 0) { // Redirecionamento de erro (stderr)
                    error_redirect = 1;
                    error_file = args[j+1];
                    args[j] = NULL;
                    j += 2;
                } else {
                    j++;
                }
            }

            if (args[0] == NULL) exit(0);

            // Redirecionamento de Pipes: liga o stdout de um comando ao stdin do comando seguinte
    if (i > 0) { // Se não for o primeiro comando, redireciona stdin para o descritor de leitura do pipe anterior
        if (dup2(pipefds[(i - 1) * 2], STDIN_FILENO) < 0) {
            perror("dup2 stdin pipe");
            exit(1);
        }
    }
    if (i < num_commands - 1) { // Se não for o último comando, redireciona stdout para o descritor de escrita do pipe seguinte
        if (dup2(pipefds[i * 2 + 1], STDOUT_FILENO) < 0) {
            perror("dup2 stdout pipe");
            exit(1);
        }
    }

            // Fechar TODOS os descritores de pipe no filho IMEDIATAMENTE após dup2
            for (int j = 0; j < 2 * num_pipes; j++) {
                close(pipefds[j]);
            }

            // Redirecionamentos explícitos (ex: > ficheiro.txt) sobrepõem os pipes
            if (input_file) {
                int fd = open(input_file, O_RDONLY);
                if (fd < 0) { perror("open input_file"); exit(1); }
                dup2(fd, STDIN_FILENO); // Substitui a entrada padrão pelo ficheiro
                close(fd);
            }
            if (output_file) {
                int fd = open(output_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) { perror("open output_file"); exit(1); }
                dup2(fd, STDOUT_FILENO); // Substitui a saída padrão pelo ficheiro
                close(fd);
            }
            if (error_redirect && error_file) {
                int fd = open(error_file, O_WRONLY | O_CREAT | O_TRUNC, 0666);
                if (fd < 0) { perror("open error_file"); exit(1); }
                dup2(fd, STDERR_FILENO); // Substitui o erro padrão pelo ficheiro
                close(fd);
            }

            execvp(args[0], args);
            perror("execvp");
            exit(1);
        } else if (pid < 0) {
            perror("fork");
            exit(1);
        }
    }

    // O processo pai fecha todos os descritores de pipes após criar os filhos
    for (int i = 0; i < 2 * num_pipes; i++) {
        close(pipefds[i]);
    }
    // O processo pai aguarda pelo término de todos os processos filhos da pipeline
    for (int i = 0; i < num_commands; i++) {
        wait(NULL);
    }
}

// Handlers
// Handler da opção -e
void handle_executar(int argc, char *argv[]) {
    char buffer[256];
    int len;

    if (argc < 4) {
        print_msg(STDERR_FILENO, "Uso: ./runner -e <user-id> \"<comando>\"\n");
        exit(1);
    }

    // Aloca memoria
    Operacoes pedido;

    // Escreve por cima do espaço para garantir
    // que não há lixo de memória que estrague o comportamento das variáveis ou strings.
    memset(&pedido, 0, sizeof(Operacoes));

    pedido.pid = getpid();
    pedido.tipo_operacao = 1; // 1 = Pedido Novo
    pedido.user_id = atoi(argv[2]);
    pedido.command_id = pedido.pid;

    // Se não houver aspas (comando simples como sleep 2), reconstrói o comando de argv[3..argc-1]
    if (argc > 4) {
        pedido.comando[0] = '\0';
        for (int i = 3; i < argc; i++) {
            strncat(pedido.comando, argv[i], sizeof(pedido.comando) - strlen(pedido.comando) - 1);
            if (i < argc - 1) {
                strncat(pedido.comando, " ", sizeof(pedido.comando) - strlen(pedido.comando) - 1);
            }
        }
    } else {
        strncpy(pedido.comando, argv[3], sizeof(pedido.comando) - 1);
        pedido.comando[sizeof(pedido.comando) - 1] = '\0';
    }

    len = sprintf(buffer, "[runner] command %d submitted\n", pedido.command_id);
    write(STDOUT_FILENO, buffer, len);

    char fifo_privado[50];
    sprintf(fifo_privado, "/tmp/runner_%d", getpid());
    unlink(fifo_privado);

    // Cria um FIFO privado para receber a autorização do Controller para começar a execução
    if (mkfifo(fifo_privado, 0666) == -1) {
        print_msg(STDERR_FILENO, "Erro ao criar o FIFO privado.\n");
        exit(1);
    }

    // Abre o FIFO do servidor para escrita para enviar o pedido
    int fd_server = open(SERVER_FIFO, O_WRONLY);
    if (fd_server == -1) {
        print_msg(STDERR_FILENO, "Erro ao abrir FIFO do controller.\n");
        unlink(fifo_privado);
        exit(1);
    }
    write(fd_server, &pedido, sizeof(Operacoes));
    close(fd_server);

    // Abre o FIFO privado para leitura e aguarda a autorização (bloqueante)
    int fd_resp = open(fifo_privado, O_RDONLY);
    if (fd_resp == -1) {
        perror("Erro ao abrir FIFO privado para leitura");
        unlink(fifo_privado);
        exit(1);
    }
    int autorizacao = 0;
    // Bloqueia à espera que o Monitor do Controller envie autorização
    if (read(fd_resp, &autorizacao, sizeof(int)) <= 0) {
        close(fd_resp);
        unlink(fifo_privado);
        exit(1);
    }
    close(fd_resp);

    if (autorizacao == 1) {
        // Reabre o FIFO privado para escrita para comunicar com o processo monitor do Controller
        int fd_aviso = open(fifo_privado, O_WRONLY);
        if (fd_aviso == -1) {
            perror("Erro ao abrir FIFO privado para escrita");
            unlink(fifo_privado);
            exit(1);
        }
        pedido.tipo_operacao = 5; // Notifica o início da execução
        write(fd_aviso, &pedido, sizeof(Operacoes));

        len = sprintf(buffer, "[runner] executing command %d...\n", pedido.command_id);
        write(STDOUT_FILENO, buffer, len);

        // Chamar a função de execução de pipeline
        execute_pipeline(pedido.comando);

        pedido.tipo_operacao = 4; // 4 = Terminado
        write(fd_aviso, &pedido, sizeof(Operacoes));
        close(fd_aviso);

        len = sprintf(buffer, "[runner] command %d finished\n", pedido.command_id);
        write(STDOUT_FILENO, buffer, len);
    }
    unlink(fifo_privado);
}

// Handler da opção -status
void handle_consultar(int argc, char *argv[]) {
    Operacoes pedido;
    memset(&pedido, 0, sizeof(Operacoes));

    pedido.pid = getpid();
    pedido.tipo_operacao = 2; // 2 = Pedido de Consulta

    char fifo_privado[50];
    sprintf(fifo_privado, "/tmp/runner_%d", getpid());
    unlink(fifo_privado);

    if (mkfifo(fifo_privado, 0666) == -1) {
        print_msg(STDERR_FILENO, "Erro ao criar o FIFO privado.\n");
        exit(1);
    }

    int fd_server = open(SERVER_FIFO, O_WRONLY);
    if (fd_server == -1) {
        print_msg(STDERR_FILENO, "Erro ao abrir FIFO do controller.\n");
        unlink(fifo_privado);
        exit(1);
    }
    write(fd_server, &pedido, sizeof(Operacoes));
    close(fd_server);

    int fd_resp = open(fifo_privado, O_RDONLY);
    char buffer[512];
    int bytes_lidos;

    while ((bytes_lidos = read(fd_resp, buffer, sizeof(buffer))) > 0) {
        write(STDOUT_FILENO, buffer, bytes_lidos);
    }

    close(fd_resp);
    unlink(fifo_privado);
}

// Handler da opção -s
void handle_shutdown(int argc, char *argv[]) {
    char buffer[256];
    int len;

    Operacoes pedido;
    memset(&pedido, 0, sizeof(Operacoes));
    pedido.pid = getpid();
    pedido.tipo_operacao = 3; // 3 = Pedido de Shutdown

    char fifo_privado[50];
    sprintf(fifo_privado, "/tmp/runner_%d", getpid());
    unlink(fifo_privado);

    if (mkfifo(fifo_privado, 0666) == -1) {
        print_msg(STDERR_FILENO, "Erro ao criar o FIFO privado.\n");
        exit(1);
    }

    int fd_server = open(SERVER_FIFO, O_WRONLY);
    if (fd_server == -1) {
        print_msg(STDERR_FILENO, "Erro ao abrir FIFO do controller.\n");
        unlink(fifo_privado);
        exit(1);
    }
    write(fd_server, &pedido, sizeof(Operacoes));
    close(fd_server);

    len = sprintf(buffer, "[runner] sent shutdown notification\n[runner] waiting for controller to shutdown....\n");
    write(STDOUT_FILENO, buffer, len);

    int fd_resp = open(fifo_privado, O_RDONLY);
    int autorizacao = 0;
    read(fd_resp, &autorizacao, sizeof(int));
    close(fd_resp);
    unlink(fifo_privado);

    len = sprintf(buffer, "[runner] controller exited.\n");
    write(STDOUT_FILENO, buffer, len);
}

int main(int argc, char *argv[]) {
    if (argc < 2) {
        print_msg(STDERR_FILENO, "Uso: ./runner <-e|-c|-s> [argumentos...]\n");
        return 1;
    }

    if (strcmp(argv[1], "-e") == 0) {
        handle_executar(argc, argv);
    }
    else if (strcmp(argv[1], "-c") == 0) {
        handle_consultar(argc, argv);
    }
    else if (strcmp(argv[1], "-s") == 0) {
        handle_shutdown(argc, argv);
    }
    else {
        print_msg(STDERR_FILENO, "Operacao Invalida. Use -e, -c ou -s.\n");
        return 1;
    }

    return 0;
}