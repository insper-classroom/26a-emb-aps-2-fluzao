# Controle Customizado — Traffic Rider

Projeto integrador da disciplina de Sistemas Embarcados (Insper). Desenvolvimento de um controle físico dedicado ao jogo **Traffic Rider** combinando os laboratórios *expert* de **IA (Edge Impulse)** e **RTOS (FreeRTOS SMP)** em uma única plataforma — Raspberry Pi Pico 2 (RP2350).

> **Status:** entrega prévia — design e arquitetura. Implementação em andamento.

---

## 1. Jogo

[**Traffic Rider**](https://www.crazygames.com/game/traffic-rider-vvq) é um jogo de corrida de moto em primeira pessoa, jogável em navegador no PC. O jogador pilota uma moto em alta velocidade desviando do trânsito, com mecânicas de aceleração, frenagem, esterçamento e wheelie.

**Mapeamento de teclas do jogo utilizado neste controle:**

| Tecla | Função |
|---|---|
| `↑` | Acelerar |
| `↓` | Frear |
| `← / →` | Esterçar (substituído pelo IMU) |
| `H` | Buzina |
| `Y` | Wheelie (acionado por gesto via IA) |
| `P` | Pausar |

---

## 2. Ideia do controle

A proposta é um controle no formato de **guidão de moto reduzido**, segurado com as duas mãos, onde a inclinação lateral do corpo do controle (eixo de roll do IMU) controla diretamente o esterçamento da moto no jogo. Botões físicos posicionados nas extremidades atendem aos comandos discretos (acelerar, frear, buzina e pause).

O diferencial do controle é o **wheelie por gesto**: ao invés de um botão dedicado, o jogador realiza o movimento físico de puxar o guidão pra trás, e um modelo de IA embarcado classifica o gesto e dispara a tecla `Y` no jogo. Isso aproxima a interação da pilotagem real e dá uso justificável ao lab de IA.

### Esboço mecânico

> Esboço/scratch a ser anexado nos próximos commits.

Direcionamento: formato de tubo/barra curto com pegadas nas extremidades, simulando a posição de pilotagem. Eletrônica (Pico 2, MPU6050, bateria) embutidas no corpo central. Botões posicionados próximos ao polegar de cada mão.

---

## 3. Inputs e Outputs

### Inputs

| Componente | Interface | Função | Tratamento |
|---|---|---|---|
| MPU6050 (IMU) | I²C | Aceleração + giroscópio para steering contínuo e detecção de gestos | Polling em task @ 100 Hz |
| Botão Acelerar | GPIO digital | Tecla `↑` | ISR + semáforo + debounce |
| Botão Frear | GPIO digital | Tecla `↓` | ISR + semáforo + debounce |
| Botão Buzina | GPIO digital | Tecla `H` | ISR + semáforo + debounce |
| Botão Pause | GPIO digital | Tecla `P` | ISR + semáforo + debounce |

### Outputs

| Componente | Interface | Função |
|---|---|---|
| LED RGB | GPIO/PWM | Indicador de status: conectado, desconectado, calibrando, ativo |
| USB | USB-HID | Comunicação com o PC (emulação de teclado) |

### Decisões pendentes (a confirmar até a entrega final)

- **Bateria** vs alimentação USB direta — definir se vai integrar gerenciamento de bateria (acumulativo +0.5)
- **Throttle analógico** — substituir o botão de acelerar por potenciômetro/gatilho ADC para acelerador progressivo (acumulativo ADC + IMU +0.5)
- **Motor de vibração** — adicionar feedback háptico para eventos locais (acumulativo +0.5)

---

## 4. Protocolo

**USB-HID Keyboard Emulation.**

A Pico 2 enumera-se como teclado USB-HID padrão. Cada comando do controle é traduzido no pressionamento/liberação da tecla correspondente do jogo. Como Traffic Rider na versão web aceita teclado mas não eixos analógicos de gamepad, o steering contínuo do IMU é convertido em **pulsos modulados** das setas `←` e `→` (duty proporcional ao ângulo de roll), aproximando o comportamento de eixo analógico.

A escolha por HID-keyboard sobre HID-gamepad também simplifica a integração: não exige configuração de mapping no jogo ou software intermediário no PC.

---

## 5. Experts aplicados

### 5.1 IA — Edge Impulse

Um modelo de classificação de gestos roda localmente na Pico 2 utilizando dados do MPU6050. O modelo é treinado na plataforma Edge Impulse e exportado como biblioteca C++ embarcada no firmware.

**Classes do modelo:**

| Classe | Significado | Ação |
|---|---|---|
| `idle` | Controle parado, posição neutra | Nenhuma |
| `steering` | Inclinação lateral sustentada (curva normal) | Nenhuma (tratado pela malha contínua de fusion) |
| `wheelie` | Movimento brusco de pull-back do controle | Dispara tecla `Y` |

**Pipeline:** janela móvel de samples do MPU6050 alimenta inferência periódica (~5 Hz). Threshold de confiança configurável para evitar falsos positivos durante curvas agressivas.

Base de código: [edgeimpulse-runner para Pico 2](https://github.com/insper-embarcados/edgeimpulse-runner) com substituição dos artefatos `tflite-model`, `model-parameters` e `edge-impulse-sdk` pelo modelo treinado.

### 5.2 RTOS — FreeRTOS SMP

Firmware estruturado em tasks com filas e semáforos (sem variáveis globais). O modo **SMP** é ativado para distribuir as tasks entre os dois cores do RP2350, isolando a inferência de IA (carga maior) do pipeline crítico de input/HID (baixa latência).

**Métricas a reportar (entregáveis do lab):**

| Métrica | imu_task | fusion_task | ai_task | button_task | hid_task |
|---|---|---|---|---|---|
| WCET | TBD | TBD | TBD | TBD | TBD |
| Jitter | TBD | TBD | TBD | TBD | TBD |
| Deadline Miss Rate | TBD | TBD | TBD | TBD | TBD |
| Stack Usage | TBD | TBD | TBD | TBD | TBD |

Medições serão realizadas duas vezes: **Single Core** e **SMP (2 cores)**, com tabela comparativa no relatório final.

---

## 6. Diagrama de blocos do firmware
<img width="1600" height="872" alt="image" src="https://github.com/user-attachments/assets/1fb867bc-fc3a-449d-8c85-8c6b38a7b7b4" />



### 6.1 Tasks

| Task | Core (afinidade) | Prioridade | Período | Função |
|---|---|---|---|---|
| `imu_task` | 0 | Alta | 10 ms (100 Hz) | Lê MPU6050 via I²C, publica samples para fusion e janela para IA |
| `fusion_task` | 1 | Média | Event-driven | Filtro complementar → pitch/roll → valor normalizado de steering |
| `ai_task` | 1 | Média-baixa | 200 ms (5 Hz) | Inferência Edge Impulse sobre janela de samples |
| `button_task` | 0 | Alta | Event-driven | Debounce e tradução de eventos de botão em comandos de teclado |
| `hid_task` | 0 | Alta | 20 ms (50 Hz) | Agrega steering + eventos discretos, monta e envia report HID |
| `led_task` | 1 | Baixa | 100 ms | Atualiza LED RGB conforme estado do sistema |

### 6.2 Filas

| Fila | Produtor → Consumidor | Conteúdo |
|---|---|---|
| `q_imu_samples` | imu_task → fusion_task | Sample bruto (ax, ay, az, gx, gy, gz) |
| `q_imu_window` | imu_task → ai_task | Buffer circular de samples para inferência |
| `q_steering` | fusion_task → hid_task | Valor normalizado de esterçamento (-1.0 a +1.0) |
| `q_btn_events` | button_task → hid_task | Eventos de press/release dos botões |
| `q_ai_events` | ai_task → hid_task | Eventos de classificação (ex: `WHEELIE_DETECTED`) |
| `q_state` | hid_task → led_task | Mudanças de estado do sistema |

### 6.3 Semáforos

| Semáforo | Tipo | Função |
|---|---|---|
| `sem_btn[4]` | Binário | Sincroniza ISR de cada botão com `button_task` |
| `mtx_i2c` | Mutex | Acesso exclusivo ao barramento I²C (uso futuro caso outro sensor seja adicionado) |

### 6.4 ISRs

| ISR | Trigger | Ação |
|---|---|---|
| `gpio_isr_btn1..4` | Borda de descida em cada GPIO de botão | `xSemaphoreGiveFromISR(sem_btn[n])` |
| USB callbacks | Eventos do TinyUSB (conexão/desconexão) | Atualiza estado e notifica `led_task` |

---

## 7. Cronograma de entregas

- [x] **Entrega prévia (15/05):** README com design e arquitetura
- [ ] **Implementação:** firmware em FreeRTOS, integração USB-HID
- [ ] **Coleta de dados e treinamento:** modelo Edge Impulse
- [ ] **Deploy do modelo:** integração na Pico 2 via `edge-impulse-runner`
- [ ] **Medições de RTOS:** Single Core + SMP, tabelas e gráficos
- [ ] **Hardware:** PCB/protótipo mecânico e bateria
- [ ] **Vídeo e entrega final**

---

## 8. Acumulativos pretendidos

Itens do regulamento da APS que o projeto pretende cumprir (a confirmar durante o desenvolvimento):

- [ ] ADC + IMU em conjunto (substituir botão de acelerar por gatilho analógico)
- [ ] Componente fora da sala de aula (motor de vibração / encoder / etc.)
- [ ] Botão de macro (gravação e reprodução de sequência)
- [ ] Vídeo top/stonks nível Kickstarter
- [ ] Controle recebe info do PC (vibração no crash)
- [ ] Calibração guiada via LED
- [ ] Háptico (motor de vibração)
- [ ] Gerenciamento de bateria

---

## 9. Referências

- [Repositório base — Edge Impulse Runner para Pico 2](https://github.com/insper-embarcados/edgeimpulse-runner)
- [Repositório base — Data Forwarder para Edge Impulse](https://github.com/insper-embarcados/edgeimpulse-dataforwarding)
- [Documentação oficial — Continuous Motion Recognition (Edge Impulse)](https://docs.edgeimpulse.com/docs/tutorials/end-to-end-tutorials/continuous-motion-recognition)
- [FreeRTOS SMP — documentação oficial](https://www.freertos.org/symmetric-multiprocessing-introduction.html)
- [Traffic Rider — CrazyGames (versão web)](https://www.crazygames.com/game/traffic-rider-vvq)

---

## 10. Equipe

> *Preencher com os membros da equipe.*
