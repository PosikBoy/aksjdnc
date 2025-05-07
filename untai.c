#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/wait.h>
#include <sys/ipc.h>
#include <sys/shm.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#define MAX_N 10          // Максимально допустимый размер матрицы
#define PORT 12345        // Не используется, но оставлен для возможных сетевых расширений
#define SHM_KEY 0x1234    // Базовый ключ для разделяемой памяти

int N; // Размер матрицы

// Структура задания для одного процесса
typedef struct {
    double A[MAX_N][MAX_N];   // Оригинальная матрица
    double I[MAX_N][MAX_N];   // Единичная матрица (будет превращена в обратную)
    int from_col, to_col;     // Диапазон столбцов, за которые отвечает процесс
    int size;                 // Размер матрицы
} Task;

// Структура результата — часть обратной матрицы
typedef struct {
    double part_inverse[MAX_N][MAX_N];  // Частичная обратная матрица
    int from_col, to_col;               // Диапазон столбцов, к которым относится эта часть
    int size;
} Result;

// === Ввод исходной матрицы и инициализация единичной матрицы ===
void input_matrix(double A[MAX_N][MAX_N], double I[MAX_N][MAX_N]) {
    printf("Введите размер квадратной матрицы (макс %d): ", MAX_N);
    scanf("%d", &N);
    if (N <= 0 || N > MAX_N) {
        fprintf(stderr, "Недопустимый размер матрицы\n");
        exit(1);
    }
    printf("Введите матрицу %dx%d построчно:\n", N, N);
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            scanf("%lf", &A[i][j]);
            I[i][j] = (i == j) ? 1.0 : 0.0; // Единичная матрица
        }
    }
}

// === Метод Гаусса-Жордана для нахождения обратной матрицы ===
// Работает только с частью столбцов, определённой в task
void gauss(Task* task, Result* result) {
    int size = task->size;
    double A[MAX_N][MAX_N], I[MAX_N][MAX_N];
    memcpy(A, task->A, sizeof(A));
    memcpy(I, task->I, sizeof(I));

    // Приведение матрицы A к единичной, I превращается в обратную
    for (int i = 0; i < size; i++) {
        double diag = A[i][i];
        for (int j = 0; j < size; j++) {
            A[i][j] /= diag;
            I[i][j] /= diag;
        }
        for (int k = 0; k < size; k++) {
            if (k == i) continue;
            double factor = A[k][i];
            for (int j = 0; j < size; j++) {
                A[k][j] -= factor * A[i][j];
                I[k][j] -= factor * I[i][j];
            }
        }
    }

    // Сохраняем только нужные столбцы из результата
    for (int i = 0; i < size; i++) {
        for (int j = task->from_col; j <= task->to_col; j++) {
            result->part_inverse[i][j] = I[i][j];
        }
    }
    result->from_col = task->from_col;
    result->to_col = task->to_col;
    result->size = task->size;
}

// === Сборка и печать полной обратной матрицы из двух результатов ===
void print_result(Result* r1, Result* r2) {
    double final[MAX_N][MAX_N];
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            final[i][j] = (j >= r1->from_col && j <= r1->to_col) ? r1->part_inverse[i][j] : r2->part_inverse[i][j];
        }
    }
    for (int i = 0; i < N; i++) {
        for (int j = 0; j < N; j++) {
            printf("%.3lf ", final[i][j]);
        }
        printf("\n");
    }
}

// === Реализация с использованием сокетов (UNIX socketpair) ===
void run_with_socket() {
    double A[MAX_N][MAX_N], I[MAX_N][MAX_N];
    input_matrix(A, I);

    int sockfd[2];
    if (socketpair(AF_UNIX, SOCK_STREAM, 0, sockfd) < 0) {
        perror("socketpair");
        exit(1);
    }

    // Первый процесс (левая часть)
    if (fork() == 0) {
        close(sockfd[0]);
        Task task;
        memcpy(task.A, A, sizeof(A));
        memcpy(task.I, I, sizeof(I));
        task.from_col = 0;
        task.to_col = N / 2 - 1;
        task.size = N;
        Result r;
        gauss(&task, &r);
        write(sockfd[1], &r, sizeof(Result));
        close(sockfd[1]);
        exit(0);
    }

    // Второй процесс (правая часть)
    if (fork() == 0) {
        close(sockfd[0]);
        Task task;
        memcpy(task.A, A, sizeof(A));
        memcpy(task.I, I, sizeof(I));
        task.from_col = N / 2;
        task.to_col = N - 1;
        task.size = N;
        Result r;
        gauss(&task, &r);
        write(sockfd[1], &r, sizeof(Result));
        close(sockfd[1]);
        exit(0);
    }

    // Родитель читает обе части и печатает результат
    close(sockfd[1]);
    Result r1, r2;
    read(sockfd[0], &r1, sizeof(Result));
    read(sockfd[0], &r2, sizeof(Result));
    printf("Обратная матрица (socket):\n");
    print_result(&r1, &r2);
    close(sockfd[0]);
}

// === Реализация с использованием именованных каналов (pipe) ===
void run_with_pipe() {
    double A[MAX_N][MAX_N], I[MAX_N][MAX_N];
    input_matrix(A, I);

    int pipefd1[2], pipefd2[2];
    pipe(pipefd1);
    pipe(pipefd2);

    // Первый процесс
    if (fork() == 0) {
        close(pipefd1[0]);
        Task task;
        memcpy(task.A, A, sizeof(A));
        memcpy(task.I, I, sizeof(I));
        task.from_col = 0;
        task.to_col = N / 2 - 1;
        task.size = N;
        Result r;
        gauss(&task, &r);
        write(pipefd1[1], &r, sizeof(Result));
        close(pipefd1[1]);
        exit(0);
    }

    // Второй процесс
    if (fork() == 0) {
        close(pipefd2[0]);
        Task task;
        memcpy(task.A, A, sizeof(A));
        memcpy(task.I, I, sizeof(I));
        task.from_col = N / 2;
        task.to_col = N - 1;
        task.size = N;
        Result r;
        gauss(&task, &r);
        write(pipefd2[1], &r, sizeof(Result));
        close(pipefd2[1]);
        exit(0);
    }

    // Родитель читает результат из обоих пайпов
    close(pipefd1[1]);
    close(pipefd2[1]);
    Result r1, r2;
    read(pipefd1[0], &r1, sizeof(Result));
    read(pipefd2[0], &r2, sizeof(Result));
    printf("Обратная матрица (pipe):\n");
    print_result(&r1, &r2);
    close(pipefd1[0]);
    close(pipefd2[0]);
}

// === Реализация с использованием разделяемой памяти (System V shared memory) ===
void run_with_shm() {
    double A[MAX_N][MAX_N], I[MAX_N][MAX_N];
    input_matrix(A, I);

    // Получаем 4 сегмента памяти: 2 на вход, 2 на результат
    int shmid1 = shmget(SHM_KEY, sizeof(Task), IPC_CREAT | 0666);
    int shmid2 = shmget(SHM_KEY + 1, sizeof(Task), IPC_CREAT | 0666);
    int shmr1 = shmget(SHM_KEY + 2, sizeof(Result), IPC_CREAT | 0666);
    int shmr2 = shmget(SHM_KEY + 3, sizeof(Result), IPC_CREAT | 0666);

    Task* task1 = (Task*)shmat(shmid1, NULL, 0);
    Task* task2 = (Task*)shmat(shmid2, NULL, 0);
    Result* r1 = (Result*)shmat(shmr1, NULL, 0);
    Result* r2 = (Result*)shmat(shmr2, NULL, 0);

    // Инициализируем задачи
    memcpy(task1->A, A, sizeof(A));
    memcpy(task1->I, I, sizeof(I));
    memcpy(task2->A, A, sizeof(A));
    memcpy(task2->I, I, sizeof(I));
    task1->from_col = 0;
    task1->to_col = N / 2 - 1;
    task1->size = N;
    task2->from_col = N / 2;
    task2->to_col = N - 1;
    task2->size = N;

    // Первый процесс
    if (fork() == 0) {
        gauss(task1, r1);
        exit(0);
    }

    // Второй процесс
    if (fork() == 0) {
        gauss(task2, r2);
        exit(0);
    }

    // Ждём завершения дочерних
    wait(NULL);
    wait(NULL);

    // Печатаем результат
    printf("Обратная матрица (shared memory):\n");
    print_result(r1, r2);

    // Очистка
    shmdt(task1); shmdt(task2);
    shmdt(r1); shmdt(r2);
    shmctl(shmid1, IPC_RMID, NULL);
    shmctl(shmid2, IPC_RMID, NULL);
    shmctl(shmr1, IPC_RMID, NULL);
    shmctl(shmr2, IPC_RMID, NULL);
}

// === Главная функция — выбор метода межпроцессного взаимодействия ===
int main(int argc, char* argv[]) {
    if (argc != 2) {
        printf("Usage: %s [pipe|shm|sock] \n", argv[0]);
        return 1;
    }

    if (strcmp(argv[1], "shm") == 0) {
        run_with_shm();
    } else if (strcmp(argv[1], "sock") == 0) {
        run_with_socket();
    } else if (strcmp(argv[1], "pipe") == 0) {
        run_with_pipe();
    } else {
        printf("Unsupported method\n");
    }
    return 0;
}
