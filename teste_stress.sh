#!/bin/bash

# Cores para o terminal
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
CYAN='\033[0;36m'
NC='\033[0m' # No Color

clear
echo -e "${CYAN}=====================================================${NC}"
echo -e "${CYAN}     BATERIA COMPLETA DE TESTES DE STRESS E CARGA    ${NC}"
echo -e "${CYAN}=====================================================${NC}"

# ---------------------------------------------------------
# FASE 0: COMPILAÇÃO E LIMPEZA
# ---------------------------------------------------------
echo -e "${YELLOW}[0] A compilar o projeto e a limpar resíduos...${NC}"
make clean > /dev/null 2>&1
make > /dev/null 2>&1
rm -f registo.csv out_*.txt erro_*.txt

# ---------------------------------------------------------
# TESTE 1: FUNCIONALIDADES BASE (CONCORRÊNCIA E FILA)
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 1: Funcionalidades Base (Limite = 2, FIFO) <<<${NC}"
echo "Descrição: 4 runners simultâneos. 2 executam logo, 2 vão para a fila."

./bin/controller 2 0 > /dev/null &
PID_CONTROLLER=$!
sleep 1

./bin/runner -e 1 sleep 4 > /dev/null &
./bin/runner -e 2 sleep 6 > /dev/null &
./bin/runner -e 3 sleep 2 > /dev/null &
./bin/runner -e 4 sleep 4 > /dev/null &

sleep 1 # Pausa para deixar os pedidos serem registados

echo -e "\n${CYAN}--- Output da Consulta intermédia (-c) ---${NC}"
./bin/runner -c
echo -e "${CYAN}------------------------------------------${NC}"

# Enviar o pedido de encerramento e esperar
./bin/runner -s > /dev/null
wait $PID_CONTROLLER
echo -e "${GREEN}Teste 1 concluído com sucesso.${NC}"


# ---------------------------------------------------------
# TESTE 2: IMPACTO DO PARALELISMO - SEQUENCIAL (LIMITE = 1)
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 2: Servidor Sequencial (Limite = 1, FIFO) <<<${NC}"
echo "Descrição: 4 runners a pedir um 'sleep 2'."
echo "Expectativa: Como o limite é 1, devem executar um a um. Tempo total ~8 segundos."

./bin/controller 1 0 > /dev/null &
PID_CONTROLLER=$!
sleep 1

START_TIME=$(date +%s)

./bin/runner -e 1 sleep 2 > /dev/null &
./bin/runner -e 2 sleep 2 > /dev/null &
./bin/runner -e 3 sleep 2 > /dev/null &
./bin/runner -e 4 sleep 2 > /dev/null &

# O shutdown vai esperar que todos terminem
./bin/runner -s > /dev/null
wait $PID_CONTROLLER

END_TIME=$(date +%s)
echo -e "${GREEN}Resultado Teste 2: Tempo total = $((END_TIME - START_TIME)) segundos.${NC}"


# ---------------------------------------------------------
# TESTE 3: IMPACTO DO PARALELISMO - CONCORRENTE (LIMITE = 4)
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 3: Servidor Concorrente Máximo (Limite = 4, FIFO) <<<${NC}"
echo "Descrição: Os mesmos 4 runners a pedir um 'sleep 2'."
echo "Expectativa: Como o limite é 4, executam todos ao mesmo tempo. Tempo total ~2 segundos."

./bin/controller 4 0 > /dev/null &
PID_CONTROLLER=$!
sleep 1

START_TIME=$(date +%s)

./bin/runner -e 1 sleep 2 > /dev/null &
./bin/runner -e 2 sleep 2 > /dev/null &
./bin/runner -e 3 sleep 2 > /dev/null &
./bin/runner -e 4 sleep 2 > /dev/null &

./bin/runner -s > /dev/null
wait $PID_CONTROLLER

END_TIME=$(date +%s)
echo -e "${GREEN}Resultado Teste 3: Tempo total = $((END_TIME - START_TIME)) segundos.${NC}"


# ---------------------------------------------------------
# TESTE 4: CONSULTA DE ESTADO SOB STRESS (POLÍTICA FAIR)
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 4: Consulta de Estado com Política Fair (Limite = 2, Fair) <<<${NC}"
echo "A iniciar servidor com Limite = 2 e a injetar 5 comandos de 5 segundos..."

./bin/controller 2 1 > /dev/null &
PID_CONTROLLER=$!
sleep 1

./bin/runner -e 1 sleep 5 > /dev/null &
./bin/runner -e 2 sleep 5 > /dev/null &
./bin/runner -e 3 sleep 5 > /dev/null &
./bin/runner -e 1 sleep 5 > /dev/null &
./bin/runner -e 2 sleep 5 > /dev/null &

sleep 1 # Dar tempo para os pedidos chegarem todos ao controller

echo -e "\n${CYAN}--- Output do pedido de Consulta (-c) ---${NC}"
./bin/runner -c
echo -e "${CYAN}-----------------------------------------${NC}\n"

echo "A enviar ordem de encerramento..."
./bin/runner -s > /dev/null
wait $PID_CONTROLLER


# ---------------------------------------------------------
# TESTE 5: VERIFICAÇÃO FINAL DOS DADOS
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> VERIFICAÇÃO FINAL: Conteúdo do registo.csv <<<${NC}"
echo -e "${CYAN}-----------------------------------------${NC}"
if [ -f registo.csv ]; then
    cat registo.csv
else
    echo -e "${RED}Erro: O ficheiro registo.csv não foi gerado.${NC}"
fi
echo -e "${CYAN}-----------------------------------------${NC}"

# ---------------------------------------------------------
# TESTE 6: CASOS DE ERRO E ROBUSTEZ (O "CRASH TEST")
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 6: Crash Test (Erros de Ficheiros e Argumentos) <<<${NC}"

./bin/controller 2 0 > /dev/null &
PID_CONTROLLER=$!
sleep 1

echo -e " -> Teste 6.1: Redirecionamento para ficheiro sem permissão"
# Tentar escrever num ficheiro do sistema (deve dar erro no runner, mas controller deve seguir)
./bin/runner -e 1 "ls > /root/teste_proibido.txt" 2> /dev/null &

echo -e " -> Teste 6.2: Pipeline com comando que falha no meio"
# O 'comando_inexistente' deve falhar, mas o 'wc -l' deve receber 0 bytes e terminar normalmente
./bin/runner -e 1 "comando_inexistente | wc -l" > out_falha.txt &

echo -e " -> Teste 6.3: Input de ficheiro que não existe"
./bin/runner -e 1 "sort < ficheiro_fantasma.txt" 2> /dev/null &

echo -e " -> Teste 6.4: Muitos argumentos (Teste de Buffer)"
# Criar uma string gigante de argumentos
GIGANTE=$(printf 'arg%.0s' {1..200})
./bin/runner -e 1 "echo $GIGANTE" > /dev/null &

sleep 2

# ---------------------------------------------------------
# TESTE 7: TENTATIVA DE INJEÇÃO PÓS-SHUTDOWN
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 7: Tentativa de Injeção em Shutdown <<<${NC}"
echo "Descrição: Enviar shutdown e, logo a seguir, tentar enviar um novo comando."

./bin/runner -s > /dev/null &
sleep 0.1
# Este comando deve ser rejeitado pelo Controller (Estado: Rejeitado)
./bin/runner -e 1 "sleep 1"

# 1. Criar um ficheiro de teste
echo -e "C\nA\nB" > lista_baguncada.txt

# 2. Correr o runner usando o < para alimentar o comando sort
./bin/runner -e 1 "sort < lista_baguncada.txt" > out_teste_input.txt

# 3. Verificar se o output foi ordenado corretamente
cat out_teste_input.txt

wait $PID_CONTROLLER
echo -e "${GREEN}Teste de Robustez concluído.${NC}"

# ---------------------------------------------------------
# TESTE 8: EXAUSTÃO E CORRUPÇÃO (RESILIÊNCIA)
# ---------------------------------------------------------
echo -e "\n${YELLOW}>>> TESTE 8: Resiliência de Recursos e Protocolo <<<${NC}"

./bin/controller 5 0 > /dev/null &
PID_CONTROLLER=$!
sleep 1

echo -e " -> Teste 8.1: Rajada de 50 processos simultâneos"
for i in {1..50}; do
   ./bin/runner -e $i "sleep 1" > /dev/null &
done

echo -e " -> Teste 8.2: Injeção de lixo no FIFO principal"
echo "Mensagem de erro propositada para testar o switch" > tmp/server_fifo 2> /dev/null

echo -e " -> Teste 8.3: Pipeline complexo com fecho prematuro"
./bin/runner -e 1 "yes | head -n 1000 | wc -l" > out_pipe.txt &

sleep 2
./bin/runner -c | grep "Executing" -A 5
./bin/runner -s > /dev/null
wait $PID_CONTROLLER
echo -e "${GREEN}Fim da bateria de testes de resiliência.${NC}"

echo -e "\n${GREEN}Todos os testes da bateria foram concluídos com sucesso!${NC}"