/*
 * TRABALHO HELENA - ORGANIZACAO SEQUENCIAL-INDEXADA
 *
 * Estrategia (igual a original):
 * - produtos.bin permanece fisicamente ordenado por id_produto.
 * - Insercao: busca binaria no arquivo e deslocamento dos registros no disco.
 * - elo = proxima posicao fisica (i + 1); o ultimo recebe -1.
 * - exclusao = 0 valido; exclusao = 1 excluido (remocao logica).
 * - Compactacao remove fisicamente os excluidos.
 * - Indice parcial: 1 entrada a cada MAX_INDICE_10000 registros ativos.
 */

#define __USE_MINGW_ANSI_STDIO 1   /* faz %lld funcionar no MinGW */
#define _FILE_OFFSET_BITS 64
#define _LARGEFILE64_SOURCE

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#define SEEK64 fseeko64
#define TELL64 ftello64
#else
#define SEEK64 fseeko
#define TELL64 ftello
#endif

typedef long long i64;

#define MAX_INDICE_PROD    100    /* mesmo valor do conversor */
#define MAX_INDICE_ACESSO  1000   /* mesmo valor do conversor */
#define TAM_BUF            (1 << 20)

#define ARQ_PRODUTOS "produtos.bin"
#define ARQ_ACESSOS "acessos.bin"
#define ARQ_INDICE_PRODUTOS "indice_produtos.bin"
#define ARQ_INDICE_ACESSOS "indice_acessos.bin"

typedef struct {
    int id_produto;
    char marca[50];
    char categoria[30];
    float preco;
    int exclusao;
    int elo;
} Produto;

typedef struct {
    int id_evento;
    int id_usuario;
    int id_produto;
    char tipo_evento[30];
    int exclusao;
    int elo;
} Acesso;

/* posicao = byte onde o registro comeca no arquivo de dados */
typedef struct {
    int id_produto;
    i64 posicao;
} IndiceProduto;

typedef struct {
    int id_evento;
    i64 posicao;
} IndiceAcesso;

IndiceProduto *indicesProdutos = NULL;
IndiceAcesso *indicesAcessos = NULL;
int totalIndicesProdutos = 0;
int totalIndicesAcessos = 0;

/* Mostra campos de texto preenchidos com espacos no final. */
void imprimirCampo(char campo[], size_t tamanho) {
    while (tamanho > 0 && (campo[tamanho - 1] == ' ' || campo[tamanho - 1] == '\0'))
        tamanho--;
    printf("%.*s", (int)tamanho, campo);
}

static void imprimirProduto(Produto *p) {
    printf("ID: %d - Marca: ", p->id_produto);
    imprimirCampo(p->marca, sizeof(p->marca));
    printf(" - Categoria: ");
    imprimirCampo(p->categoria, sizeof(p->categoria));
    printf(" - Preco: R$ %.2f\n", p->preco);
}

static void imprimirAcesso(Acesso *a) {
    printf("ID evento: %d - Usuario: %d - Produto: %d - Tipo: ",
           a->id_evento, a->id_usuario, a->id_produto);
    imprimirCampo(a->tipo_evento, sizeof(a->tipo_evento));
    printf("\n");
}

static int contarProdutos(FILE *arquivo, i64 *quantidade) {
    i64 tamanho;
    if (SEEK64(arquivo, 0, SEEK_END) != 0)
        return 0;
    tamanho = TELL64(arquivo);
    if (tamanho < 0 || tamanho % (i64)sizeof(Produto) != 0)
        return 0;
    *quantidade = tamanho / (i64)sizeof(Produto);
    return 1;
}

static int contarAcessos(FILE *arquivo, i64 *quantidade) {
    i64 tamanho;
    if (SEEK64(arquivo, 0, SEEK_END) != 0)
        return 0;
    tamanho = TELL64(arquivo);
    if (tamanho < 0 || tamanho % (i64)sizeof(Acesso) != 0)
        return 0;
    *quantidade = tamanho / (i64)sizeof(Acesso);
    return 1;
}

static int lerProduto(FILE *arquivo, i64 posicao, Produto *produto) {
    if (SEEK64(arquivo, posicao * (i64)sizeof(Produto), SEEK_SET) != 0)
        return 0;
    return fread(produto, sizeof(Produto), 1, arquivo) == 1;
}

static int gravarProduto(FILE *arquivo, i64 posicao, const Produto *produto) {
    if (SEEK64(arquivo, posicao * (i64)sizeof(Produto), SEEK_SET) != 0)
        return 0;
    return fwrite(produto, sizeof(Produto), 1, arquivo) == 1;
}

static int lerAcesso(FILE *arquivo, i64 posicao, Acesso *acesso) {
    if (SEEK64(arquivo, posicao * (i64)sizeof(Acesso), SEEK_SET) != 0)
        return 0;
    return fread(acesso, sizeof(Acesso), 1, arquivo) == 1;
}

/* Troca um arquivo por seu temporario, preservando backup em caso de erro. */
static int substituirArquivo(const char *temporario, const char *destino,
                             const char *backup) {
    FILE *teste;
    remove(backup);

    if (rename(destino, backup) != 0) {
        teste = fopen(destino, "rb");
        if (teste != NULL) {
            fclose(teste);
            printf("Erro ao criar backup de %s.\n", destino);
            return 0;
        }
    }

    if (rename(temporario, destino) != 0) {
        perror("Erro ao renomear arquivo temporario");
        rename(backup, destino);
        return 0;
    }

    remove(backup);
    return 1;
}

/* Confere a ordenacao fisica sem carregar o arquivo inteiro na RAM. */
static int produtosEstaoOrdenados(FILE *arquivo, i64 quantidade) {
    Produto produto;
    i64 i;
    int primeiro = 1;
    int idAnterior = 0;

    for (i = 0; i < quantidade; i++) {
        if (!lerProduto(arquivo, i, &produto))
            return 0;
        if (!primeiro && produto.id_produto <= idAnterior)
            return 0;
        idAnterior = produto.id_produto;
        primeiro = 0;
    }
    return 1;
}

/* Atualiza elo para refletir a sequencia fisica sem area de extensao. */
static int atualizarElosProdutos(void) {
    FILE *arquivo = fopen(ARQ_PRODUTOS, "r+b");
    i64 quantidade, i;
    Produto produto;

    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return 0;
    }
    if (!contarProdutos(arquivo, &quantidade)) {
        printf("Arquivo de produtos invalido.\n");
        fclose(arquivo);
        return 0;
    }

    for (i = 0; i < quantidade; i++) {
        if (!lerProduto(arquivo, i, &produto)) {
            printf("Erro ao ler produto para atualizar elo.\n");
            fclose(arquivo);
            return 0;
        }
        produto.elo = (i + 1 < quantidade) ? (int)(i + 1) : -1;
        if (!gravarProduto(arquivo, i, &produto)) {
            printf("Erro ao atualizar elo do produto.\n");
            fclose(arquivo);
            return 0;
        }
    }
    fclose(arquivo);
    return 1;
}

/* Le o arquivo de indice para a memoria. Retorna 0 se nao existir/invalido. */
static int recarregarIndiceProdutos(void) {
    FILE *arquivo;
    IndiceProduto *novo = NULL;
    i64 tamanho;
    int total;

    arquivo = fopen(ARQ_INDICE_PRODUTOS, "rb");
    if (arquivo == NULL)
        return 0;
    if (SEEK64(arquivo, 0, SEEK_END) != 0) {
        fclose(arquivo);
        return 0;
    }
    tamanho = TELL64(arquivo);
    rewind(arquivo);
    if (tamanho < 0 || tamanho % (i64)sizeof(IndiceProduto) != 0) {
        fclose(arquivo);
        return 0;
    }

    total = (int)(tamanho / (i64)sizeof(IndiceProduto));
    if (total > 0) {
        novo = (IndiceProduto *)malloc((size_t)total * sizeof(IndiceProduto));
        if (novo == NULL) {
            fclose(arquivo);
            return 0;
        }
        if (fread(novo, sizeof(IndiceProduto), (size_t)total, arquivo) != (size_t)total) {
            free(novo);
            fclose(arquivo);
            return 0;
        }
    }
    fclose(arquivo);
    free(indicesProdutos);
    indicesProdutos = novo;
    totalIndicesProdutos = total;
    return 1;
}

static int recarregarIndiceAcessos(void) {
    FILE *arquivo;
    IndiceAcesso *novo = NULL;
    i64 tamanho;
    int total;

    arquivo = fopen(ARQ_INDICE_ACESSOS, "rb");
    if (arquivo == NULL)
        return 0;
    if (SEEK64(arquivo, 0, SEEK_END) != 0) {
        fclose(arquivo);
        return 0;
    }
    tamanho = TELL64(arquivo);
    rewind(arquivo);
    if (tamanho < 0 || tamanho % (i64)sizeof(IndiceAcesso) != 0) {
        fclose(arquivo);
        return 0;
    }

    total = (int)(tamanho / (i64)sizeof(IndiceAcesso));
    if (total > 0) {
        novo = (IndiceAcesso *)malloc((size_t)total * sizeof(IndiceAcesso));
        if (novo == NULL) {
            fclose(arquivo);
            return 0;
        }
        if (fread(novo, sizeof(IndiceAcesso), (size_t)total, arquivo) != (size_t)total) {
            free(novo);
            fclose(arquivo);
            return 0;
        }
    }
    fclose(arquivo);
    free(indicesAcessos);
    indicesAcessos = novo;
    totalIndicesAcessos = total;
    return 1;
}

/* Reconstroi o indice lendo os produtos em ordem fisica (leitura sequencial). */
void reconstruirIndiceProdutos(void) {
    FILE *arquivo = NULL;
    FILE *indice = NULL;
    Produto produto;
    IndiceProduto entrada;
    i64 quantidade = 0, posicao = 0, ativos = 0;
    int primeiro = 1;
    int idAnterior = 0;

    arquivo = fopen(ARQ_PRODUTOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    if (!contarProdutos(arquivo, &quantidade)) {
        printf("Arquivo de produtos invalido.\n");
        fclose(arquivo);
        return;
    }
    rewind(arquivo);
    setvbuf(arquivo, NULL, _IOFBF, TAM_BUF);

    indice = fopen("indice_produtos.tmp", "wb");
    if (indice == NULL) {
        perror("Erro ao criar indice temporario");
        fclose(arquivo);
        return;
    }

    while (fread(&produto, sizeof(Produto), 1, arquivo) == 1) {
        if (!primeiro && produto.id_produto <= idAnterior) {
            printf("Erro: produtos.bin nao esta ordenado por id_produto.\n");
            printf("Gere novamente produtos.bin com o conversor ordenado antes de usar esta versao.\n");
            goto erro;
        }
        primeiro = 0;
        idAnterior = produto.id_produto;

        if (produto.exclusao == 0) {
            if (ativos % MAX_INDICE_PROD == 0) {
                memset(&entrada, 0, sizeof(entrada));
                entrada.id_produto = produto.id_produto;
                entrada.posicao = posicao * (i64)sizeof(Produto);   /* em bytes */
                if (fwrite(&entrada, sizeof(IndiceProduto), 1, indice) != 1) {
                    printf("Erro ao gravar indice de produtos.\n");
                    goto erro;
                }
            }
            ativos++;
        }
        posicao++;
    }
    if (ferror(arquivo)) {
        printf("Erro ao ler produtos.bin.\n");
        goto erro;
    }

    fclose(arquivo);
    arquivo = NULL;
    if (fclose(indice) != 0) {
        indice = NULL;
        remove("indice_produtos.tmp");
        return;
    }
    indice = NULL;

    if (!substituirArquivo("indice_produtos.tmp", ARQ_INDICE_PRODUTOS,
                           "indice_produtos.bak")) {
        remove("indice_produtos.tmp");
        return;
    }
    if (!recarregarIndiceProdutos()) {
        printf("Erro ao recarregar indice de produtos na memoria.\n");
        return;
    }
    printf("Indice de produtos reconstruido. Ativos: %lld; entradas: %d.\n",
           ativos, totalIndicesProdutos);
    return;

erro:
    if (arquivo != NULL) fclose(arquivo);
    if (indice != NULL) fclose(indice);
    remove("indice_produtos.tmp");
}

void reconstruirIndiceAcessos(void) {
    FILE *arquivo = NULL, *indice = NULL;
    Acesso acesso;
    IndiceAcesso entrada;
    i64 quantidade = 0, posicao = 0, ativos = 0;
    int primeiro = 1, idAnterior = 0;

    arquivo = fopen(ARQ_ACESSOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo acessos.bin nao encontrado.\n");
        return;
    }
    if (!contarAcessos(arquivo, &quantidade)) {
        printf("Arquivo de acessos invalido (tamanho nao e multiplo de %d bytes?).\n",
               (int)sizeof(Acesso));
        fclose(arquivo);
        return;
    }
    rewind(arquivo);
    setvbuf(arquivo, NULL, _IOFBF, TAM_BUF);

    indice = fopen("indice_acessos.tmp", "wb");
    if (indice == NULL) {
        perror("Erro ao criar indice de acessos");
        fclose(arquivo);
        return;
    }

    printf("Reconstruindo indice de acessos (%lld registros)...\n", quantidade);
    while (fread(&acesso, sizeof(Acesso), 1, arquivo) == 1) {
        if (!primeiro && acesso.id_evento <= idAnterior) {
            printf("Erro: acessos.bin nao esta ordenado por id_evento.\n");
            goto erro;
        }
        primeiro = 0;
        idAnterior = acesso.id_evento;

        if (acesso.exclusao == 0) {
            if (ativos % MAX_INDICE_ACESSO == 0) {
                memset(&entrada, 0, sizeof(entrada));
                entrada.id_evento = acesso.id_evento;
                entrada.posicao = posicao * (i64)sizeof(Acesso);    /* em bytes */
                if (fwrite(&entrada, sizeof(IndiceAcesso), 1, indice) != 1) {
                    printf("Erro ao gravar indice de acessos.\n");
                    goto erro;
                }
            }
            ativos++;
        }
        posicao++;
    }
    if (ferror(arquivo)) {
        printf("Erro ao ler acessos.bin.\n");
        goto erro;
    }

    fclose(arquivo);
    arquivo = NULL;
    if (fclose(indice) != 0) {
        indice = NULL;
        remove("indice_acessos.tmp");
        return;
    }
    indice = NULL;
    if (!substituirArquivo("indice_acessos.tmp", ARQ_INDICE_ACESSOS, "indice_acessos.bak")) {
        remove("indice_acessos.tmp");
        return;
    }
    if (!recarregarIndiceAcessos())
        printf("Erro ao recarregar indice de acessos na memoria.\n");
    else
        printf("Indice de acessos reconstruido. Ativos: %lld; entradas: %d.\n",
               ativos, totalIndicesAcessos);
    return;

erro:
    if (arquivo != NULL) fclose(arquivo);
    if (indice != NULL) fclose(indice);
    remove("indice_acessos.tmp");
}

/* Na abertura: so CARREGA os indices. Reconstroi apenas se nao existirem. */
void carregarIndices(void) {
    if (!recarregarIndiceProdutos()) {
        printf("Indice de produtos nao encontrado; reconstruindo...\n");
        reconstruirIndiceProdutos();
    } else {
        printf("Indice de produtos carregado: %d entradas.\n", totalIndicesProdutos);
    }

    if (!recarregarIndiceAcessos()) {
        printf("Indice de acessos nao encontrado; reconstruindo...\n");
        reconstruirIndiceAcessos();
    } else {
        printf("Indice de acessos carregado: %d entradas.\n", totalIndicesAcessos);
    }
}

void mostrarProdutos(void) {
    FILE *arquivo = fopen(ARQ_PRODUTOS, "rb");
    Produto produto;
    int contador = 0, limite;

    if (arquivo == NULL) {
        printf("Arquivo de produtos nao encontrado.\n");
        return;
    }
    printf("Quantos produtos mostrar (0 = todos): ");
    if (scanf("%d", &limite) != 1) {
        fclose(arquivo);
        return;
    }
    setvbuf(arquivo, NULL, _IOFBF, TAM_BUF);
    printf("\n========== PRODUTOS (ORDEM FISICA) ==========\n");
    while ((limite == 0 || contador < limite) &&
           fread(&produto, sizeof(Produto), 1, arquivo) == 1) {
        if (produto.exclusao == 0) {
            imprimirProduto(&produto);
            contador++;
        }
    }
    if (ferror(arquivo)) printf("Erro durante a leitura de produtos.bin.\n");
    if (contador == 0) printf("Nenhum produto ativo encontrado.\n");
    else printf("Produtos exibidos: %d\n", contador);
    fclose(arquivo);
}

void mostrarAcessos(void) {
    FILE *arquivo = fopen(ARQ_ACESSOS, "rb");
    Acesso acesso;
    int contador = 0, limite;

    if (arquivo == NULL) {
        printf("Arquivo de acessos nao encontrado.\n");
        return;
    }
    printf("Quantos acessos mostrar (0 = todos): ");
    if (scanf("%d", &limite) != 1) {
        fclose(arquivo);
        return;
    }
    setvbuf(arquivo, NULL, _IOFBF, TAM_BUF);
    printf("\n========== ACESSOS ==========\n");
    while ((limite == 0 || contador < limite) &&
           fread(&acesso, sizeof(Acesso), 1, arquivo) == 1) {
        if (acesso.exclusao == 0) {
            imprimirAcesso(&acesso);
            contador++;
        }
    }
    if (ferror(arquivo)) printf("Erro durante a leitura de acessos.bin.\n");
    if (contador == 0) printf("Nenhum acesso ativo encontrado.\n");
    else printf("Acessos exibidos: %d\n", contador);
    fclose(arquivo);
}

/* Consulta pelo indice parcial: busca binaria no indice (memoria),
 * seek no arquivo de dados e leitura sequencial curta a partir da entrada. */
void consultarIndiceProduto(void) {
    FILE *arquivo;
    Produto produto;
    int id, i, lo, hi, meio, e = -1, achou = 0;

    printf("\n========== CONSULTA PELO INDICE DE PRODUTOS ==========\n");
    if (indicesProdutos == NULL || totalIndicesProdutos == 0) {
        printf("Nenhuma entrada do indice de produtos carregada.\n");
        printf("Use a opcao 1 para reconstruir os indices.\n");
        return;
    }
    printf("ID do produto (0 = listar as entradas do indice): ");
    if (scanf("%d", &id) != 1) {
        printf("ID invalido.\n");
        return;
    }
    if (id == 0) {
        for (i = 0; i < totalIndicesProdutos; i++)
            printf("ID: %d - Posicao (byte): %lld\n",
                   indicesProdutos[i].id_produto, indicesProdutos[i].posicao);
        printf("Total de entradas: %d\n", totalIndicesProdutos);
        return;
    }

    /* maior entrada do indice com id <= id procurado */
    lo = 0;
    hi = totalIndicesProdutos - 1;
    while (lo <= hi) {
        meio = lo + (hi - lo) / 2;
        if (indicesProdutos[meio].id_produto <= id) {
            e = meio;
            lo = meio + 1;
        } else {
            hi = meio - 1;
        }
    }
    if (e < 0) {
        printf("Produto nao encontrado (ID menor que o primeiro do indice).\n");
        return;
    }

    arquivo = fopen(ARQ_PRODUTOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    if (SEEK64(arquivo, indicesProdutos[e].posicao, SEEK_SET) != 0) {
        printf("Erro de seek no arquivo de produtos.\n");
        fclose(arquivo);
        return;
    }
    while (fread(&produto, sizeof(Produto), 1, arquivo) == 1) {
        if (produto.id_produto == id) {
            achou = 1;
            break;
        }
        if (produto.id_produto > id)
            break;
    }
    if (!achou) printf("Produto nao encontrado.\n");
    else if (produto.exclusao == 1) printf("O produto existe, mas foi excluido logicamente.\n");
    else imprimirProduto(&produto);
    fclose(arquivo);
}

void consultarIndiceAcesso(void) {
    FILE *arquivo;
    Acesso acesso;
    int id, i, lo, hi, meio, e = -1, achou = 0;

    printf("\n========== CONSULTA PELO INDICE DE ACESSOS ==========\n");
    if (indicesAcessos == NULL || totalIndicesAcessos == 0) {
        printf("Nenhuma entrada do indice de acessos carregada.\n");
        printf("Use a opcao 1 para reconstruir os indices.\n");
        return;
    }
    printf("ID do evento (0 = listar as entradas do indice): ");
    if (scanf("%d", &id) != 1) {
        printf("ID invalido.\n");
        return;
    }
    if (id == 0) {
        for (i = 0; i < totalIndicesAcessos; i++)
            printf("ID evento: %d - Posicao (byte): %lld\n",
                   indicesAcessos[i].id_evento, indicesAcessos[i].posicao);
        printf("Total de entradas: %d\n", totalIndicesAcessos);
        return;
    }

    lo = 0;
    hi = totalIndicesAcessos - 1;
    while (lo <= hi) {
        meio = lo + (hi - lo) / 2;
        if (indicesAcessos[meio].id_evento <= id) {
            e = meio;
            lo = meio + 1;
        } else {
            hi = meio - 1;
        }
    }
    if (e < 0) {
        printf("Acesso nao encontrado (ID menor que o primeiro do indice).\n");
        return;
    }

    arquivo = fopen(ARQ_ACESSOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo acessos.bin nao encontrado.\n");
        return;
    }
    if (SEEK64(arquivo, indicesAcessos[e].posicao, SEEK_SET) != 0) {
        printf("Erro de seek no arquivo de acessos.\n");
        fclose(arquivo);
        return;
    }
    while (fread(&acesso, sizeof(Acesso), 1, arquivo) == 1) {
        if (acesso.id_evento == id) {
            achou = 1;
            break;
        }
        if (acesso.id_evento > id)
            break;
    }
    if (!achou) printf("Acesso nao encontrado.\n");
    else if (acesso.exclusao == 1) printf("O acesso existe, mas foi excluido logicamente.\n");
    else imprimirAcesso(&acesso);
    fclose(arquivo);
}

void pesquisarProduto(void) {
    FILE *arquivo;
    Produto produto;
    i64 quantidade, baixo, alto, meio;
    int id, encontrado = 0;

    printf("ID do produto: ");
    if (scanf("%d", &id) != 1) {
        printf("ID invalido.\n");
        return;
    }
    arquivo = fopen(ARQ_PRODUTOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    if (!contarProdutos(arquivo, &quantidade)) {
        printf("Arquivo de produtos invalido.\n");
        fclose(arquivo);
        return;
    }

    baixo = 0;
    alto = quantidade - 1;
    while (baixo <= alto) {
        meio = baixo + (alto - baixo) / 2;
        if (!lerProduto(arquivo, meio, &produto)) {
            printf("Erro ao ler produto durante busca binaria.\n");
            fclose(arquivo);
            return;
        }
        if (produto.id_produto == id) {
            encontrado = 1;
            if (produto.exclusao == 1) {
                printf("O produto existe, mas foi excluido logicamente.\n");
            } else {
                printf("Produto encontrado na posicao %lld.\n", meio);
                imprimirProduto(&produto);
            }
            break;
        }
        if (produto.id_produto < id) baixo = meio + 1;
        else alto = meio - 1;
    }
    if (!encontrado) printf("Produto nao encontrado.\n");
    fclose(arquivo);
}

void pesquisarAcesso(void) {
    FILE *arquivo;
    Acesso acesso;
    i64 quantidade, baixo, alto, meio;
    int id, encontrado = 0;

    printf("ID do evento: ");
    if (scanf("%d", &id) != 1) {
        printf("ID invalido.\n");
        return;
    }
    arquivo = fopen(ARQ_ACESSOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo acessos.bin nao encontrado.\n");
        return;
    }
    if (!contarAcessos(arquivo, &quantidade)) {
        printf("Arquivo de acessos invalido.\n");
        fclose(arquivo);
        return;
    }

    baixo = 0;
    alto = quantidade - 1;
    while (baixo <= alto) {
        meio = baixo + (alto - baixo) / 2;
        if (!lerAcesso(arquivo, meio, &acesso)) {
            printf("Erro ao ler acesso durante busca binaria.\n");
            fclose(arquivo);
            return;
        }
        if (acesso.id_evento == id) {
            encontrado = 1;
            if (acesso.exclusao == 1) {
                printf("O acesso existe, mas foi excluido logicamente.\n");
            } else {
                printf("Acesso encontrado na posicao %lld.\n", meio);
                imprimirAcesso(&acesso);
            }
            break;
        }
        if (acesso.id_evento < id) baixo = meio + 1;
        else alto = meio - 1;
    }
    if (!encontrado) printf("Acesso nao encontrado.\n");
    fclose(arquivo);
}

/* Insere fisicamente na posicao ordenada, deslocando registros no disco. */
void inserirProduto(void) {
    FILE *arquivo;
    Produto novo, produto;
    i64 quantidade, baixo, alto, meio, posicaoInsercao, i;

    memset(&novo, 0, sizeof(novo));
    printf("ID do produto: ");
    if (scanf("%d", &novo.id_produto) != 1) {
        printf("ID invalido.\n");
        return;
    }
    printf("Marca: ");
    if (scanf(" %49[^\n]", novo.marca) != 1) {
        printf("Marca invalida.\n");
        return;
    }
    printf("Categoria: ");
    if (scanf(" %29[^\n]", novo.categoria) != 1) {
        printf("Categoria invalida.\n");
        return;
    }
    printf("Preco: ");
    if (scanf("%f", &novo.preco) != 1 || novo.preco < 0) {
        printf("Preco invalido.\n");
        return;
    }

    arquivo = fopen(ARQ_PRODUTOS, "r+b");
    if (arquivo == NULL) arquivo = fopen(ARQ_PRODUTOS, "w+b");
    if (arquivo == NULL) {
        perror("Erro ao abrir produtos.bin");
        return;
    }
    if (!contarProdutos(arquivo, &quantidade)) {
        printf("Arquivo de produtos invalido.\n");
        fclose(arquivo);
        return;
    }
    if (!produtosEstaoOrdenados(arquivo, quantidade)) {
        printf("Nao e possivel inserir: produtos.bin nao esta ordenado fisicamente.\n");
        printf("Gere novamente produtos.bin com o conversor ordenado ou reorganize o arquivo.\n");
        fclose(arquivo);
        return;
    }

    /* Lower-bound: acha a primeira posicao cujo ID seja >= ao novo ID. */
    baixo = 0;
    alto = quantidade;
    while (baixo < alto) {
        meio = baixo + (alto - baixo) / 2;
        if (!lerProduto(arquivo, meio, &produto)) {
            printf("Erro ao ler produto durante busca binaria.\n");
            fclose(arquivo);
            return;
        }
        if (produto.id_produto < novo.id_produto) baixo = meio + 1;
        else alto = meio;
    }
    posicaoInsercao = baixo;

    if (posicaoInsercao < quantidade) {
        if (!lerProduto(arquivo, posicaoInsercao, &produto)) {
            printf("Erro ao verificar duplicidade.\n");
            fclose(arquivo);
            return;
        }
        if (produto.id_produto == novo.id_produto) {
            printf("Ja existe um registro com esse ID (ativo ou excluido).\n");
            fclose(arquivo);
            return;
        }
    }

    /* Abre espaco no arquivo deslocando do fim para a posicao de insercao. */
    for (i = quantidade; i > posicaoInsercao; i--) {
        if (!lerProduto(arquivo, i - 1, &produto)) {
            printf("Erro ao deslocar registro.\n");
            fclose(arquivo);
            return;
        }
        if (!gravarProduto(arquivo, i, &produto)) {
            printf("Erro ao gravar registro deslocado.\n");
            fclose(arquivo);
            return;
        }
    }

    novo.exclusao = 0;
    novo.elo = -1;
    if (!gravarProduto(arquivo, posicaoInsercao, &novo)) {
        printf("Erro ao gravar novo produto.\n");
        fclose(arquivo);
        return;
    }

    fclose(arquivo);

    if (!atualizarElosProdutos()) {
        printf("Produto inserido, mas houve erro ao atualizar os elos.\n");
        return;
    }
    printf("Produto inserido na posicao fisica %lld.\n", posicaoInsercao);
    printf("O arquivo continua ordenado; nao foi usada area de extensao.\n");
    reconstruirIndiceProdutos();
}

void removerProduto(void) {
    FILE *arquivo;
    Produto produto;
    i64 quantidade, baixo, alto, meio;
    int id;

    printf("ID do produto para remover: ");
    if (scanf("%d", &id) != 1) {
        printf("ID invalido.\n");
        return;
    }
    arquivo = fopen(ARQ_PRODUTOS, "r+b");
    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    if (!contarProdutos(arquivo, &quantidade)) {
        printf("Arquivo de produtos invalido.\n");
        fclose(arquivo);
        return;
    }

    baixo = 0;
    alto = quantidade - 1;
    while (baixo <= alto) {
        meio = baixo + (alto - baixo) / 2;
        if (!lerProduto(arquivo, meio, &produto)) {
            printf("Erro ao ler produto.\n");
            fclose(arquivo);
            return;
        }
        if (produto.id_produto == id) {
            if (produto.exclusao == 1) {
                printf("Esse produto ja esta excluido.\n");
            } else {
                produto.exclusao = 1;
                if (!gravarProduto(arquivo, meio, &produto)) {
                    printf("Erro ao marcar produto como excluido.\n");
                    fclose(arquivo);
                    return;
                }
                printf("Produto removido logicamente.\n");
            }
            fclose(arquivo);
            reconstruirIndiceProdutos();
            return;
        }
        if (produto.id_produto < id) baixo = meio + 1;
        else alto = meio - 1;
    }
    fclose(arquivo);
    printf("Produto nao encontrado.\n");
}

/* Compactacao opcional: remove fisicamente os excluidos, preservando a ordem. */
void compactarArquivoProdutos(void) {
    FILE *origem = NULL, *temporario = NULL, *atualizar = NULL;
    Produto produto;
    i64 mantidos = 0, i;

    origem = fopen(ARQ_PRODUTOS, "rb");
    if (origem == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    temporario = fopen("produtos.tmp", "wb");
    if (temporario == NULL) {
        perror("Erro ao criar produtos.tmp");
        fclose(origem);
        return;
    }

    while (fread(&produto, sizeof(Produto), 1, origem) == 1) {
        if (produto.exclusao == 0) {
            produto.elo = -1;
            if (fwrite(&produto, sizeof(Produto), 1, temporario) != 1) {
                printf("Erro ao gravar produtos.tmp.\n");
                fclose(origem);
                fclose(temporario);
                remove("produtos.tmp");
                return;
            }
            mantidos++;
        }
    }
    if (ferror(origem)) {
        printf("Erro ao ler produtos.bin.\n");
        fclose(origem);
        fclose(temporario);
        remove("produtos.tmp");
        return;
    }
    fclose(origem);
    fclose(temporario);

    if (!substituirArquivo("produtos.tmp", ARQ_PRODUTOS, "produtos.bak")) {
        remove("produtos.tmp");
        return;
    }

    /* Refaz elos para que cada registro aponte para o proximo fisico. */
    atualizar = fopen(ARQ_PRODUTOS, "r+b");
    if (atualizar == NULL) {
        perror("Erro ao reabrir produtos.bin");
        return;
    }
    for (i = 0; i < mantidos; i++) {
        if (!lerProduto(atualizar, i, &produto)) {
            printf("Erro ao ler produto durante compactacao.\n");
            fclose(atualizar);
            return;
        }
        produto.elo = (i + 1 < mantidos) ? (int)(i + 1) : -1;
        if (!gravarProduto(atualizar, i, &produto)) {
            printf("Erro ao atualizar elo durante compactacao.\n");
            fclose(atualizar);
            return;
        }
    }
    fclose(atualizar);

    printf("Compactacao concluida. Produtos ativos mantidos: %lld.\n", mantidos);
    reconstruirIndiceProdutos();
}

/* ------------------------------------------------------------------ */
/* MARCAS COM MAIS PRODUTOS                                            */
/* Le produtos.bin sequencialmente (1 registro por vez). Na memoria fica */
/* so a tabela de contagem por marca (poucas centenas de entradas), nao  */
/* os registros do arquivo.                                              */
/* ------------------------------------------------------------------ */

typedef struct {
    char marca[50];
    i64 qtd;
} ContagemMarca;

/* copia a marca sem os espacos/zeros do final (para comparar corretamente) */
static void limparMarca(char *destino, const char *campo) {
    size_t i = 0, n;

    while (i < 49 && campo[i] != '\0') {
        destino[i] = campo[i];
        i++;
    }
    destino[i] = '\0';
    n = i;
    while (n > 0 && destino[n - 1] == ' ') {
        n--;
        destino[n] = '\0';
    }
}

/* ordem: mais produtos primeiro; empate = ordem alfabetica */
static int cmpContagemMarca(const void *a, const void *b) {
    const ContagemMarca *x = (const ContagemMarca *)a;
    const ContagemMarca *y = (const ContagemMarca *)b;

    if (x->qtd != y->qtd)
        return (y->qtd > x->qtd) - (y->qtd < x->qtd);
    return strcmp(x->marca, y->marca);
}

void mostrarMarcasComMaisProdutos(void) {
    FILE *arquivo;
    Produto produto;
    ContagemMarca *marcas = NULL, *novo;
    char nome[50];
    int total = 0, capacidade = 0, i, achou, top;
    i64 ativos = 0;

    printf("Quantas marcas mostrar (0 = todas): ");
    if (scanf("%d", &top) != 1 || top < 0) {
        printf("Valor invalido.\n");
        return;
    }

    arquivo = fopen(ARQ_PRODUTOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    setvbuf(arquivo, NULL, _IOFBF, TAM_BUF);

    while (fread(&produto, sizeof(Produto), 1, arquivo) == 1) {
        if (produto.exclusao != 0)
            continue;                       /* ignora excluidos */
        ativos++;

        limparMarca(nome, produto.marca);
        if (nome[0] == '\0')
            strcpy(nome, "(sem marca)");

        /* procura a marca na tabela de contagem */
        achou = -1;
        for (i = 0; i < total; i++) {
            if (strcmp(marcas[i].marca, nome) == 0) {
                achou = i;
                break;
            }
        }

        if (achou >= 0) {
            marcas[achou].qtd++;
        } else {
            if (total == capacidade) {
                capacidade = (capacidade == 0) ? 64 : capacidade * 2;
                novo = (ContagemMarca *)realloc(marcas,
                                (size_t)capacidade * sizeof(ContagemMarca));
                if (novo == NULL) {
                    printf("Erro de memoria.\n");
                    free(marcas);
                    fclose(arquivo);
                    return;
                }
                marcas = novo;
            }
            strcpy(marcas[total].marca, nome);
            marcas[total].qtd = 1;
            total++;
        }
    }

    if (ferror(arquivo)) {
        printf("Erro ao ler produtos.bin.\n");
        free(marcas);
        fclose(arquivo);
        return;
    }
    fclose(arquivo);

    if (total == 0) {
        printf("Nenhum produto ativo encontrado.\n");
        free(marcas);
        return;
    }

    qsort(marcas, (size_t)total, sizeof(ContagemMarca), cmpContagemMarca);

    if (top == 0 || top > total)
        top = total;

    printf("\n========== MARCAS COM MAIS PRODUTOS ==========\n");
    printf("Produtos ativos: %lld | Marcas diferentes: %d\n\n", ativos, total);
    for (i = 0; i < top; i++) {
        printf("%3d. %-25s %8lld produtos (%.1f%%)\n",
               i + 1, marcas[i].marca, marcas[i].qtd,
               100.0 * (double)marcas[i].qtd / (double)ativos);
    }
    free(marcas);
}

/* Mostra o produto ativo com o maior preco (leitura sequencial, 1 por vez) */
void mostrarProdutoMaisCaro(void) {
    FILE *arquivo;
    Produto produto;
    Produto maisCaro;
    int encontrou = 0;

    memset(&maisCaro, 0, sizeof(maisCaro));

    arquivo = fopen(ARQ_PRODUTOS, "rb");
    if (arquivo == NULL) {
        printf("Arquivo produtos.bin nao encontrado.\n");
        return;
    }
    setvbuf(arquivo, NULL, _IOFBF, TAM_BUF);

    /* Le um produto por vez */
    while (fread(&produto, sizeof(Produto), 1, arquivo) == 1) {
        /* Considera somente produtos validos */
        if (produto.exclusao == 0) {
            if (!encontrou || produto.preco > maisCaro.preco) {
                maisCaro = produto;
                encontrou = 1;
            }
        }
    }

    if (ferror(arquivo)) {
        printf("Erro ao ler produtos.bin.\n");
        fclose(arquivo);
        return;
    }
    fclose(arquivo);

    if (!encontrou) {
        printf("Nenhum produto ativo encontrado.\n");
        return;
    }

    /* Mostra os dados do produto mais caro */
    printf("\n========== PRODUTO MAIS CARO ==========\n");
    printf("ID: %d\n", maisCaro.id_produto);

    printf("Marca: ");
    imprimirCampo(maisCaro.marca, sizeof(maisCaro.marca));

    printf("\nCategoria: ");
    imprimirCampo(maisCaro.categoria, sizeof(maisCaro.categoria));

    printf("\nPreco: R$ %.2f\n", maisCaro.preco);
}

int main(void) {
    int opcao;

    carregarIndices();
    
    do {
        printf("\n====================================\n");
        printf(" SISTEMA E-COMMERCE\n");
        printf(" Organizacao Sequencial-Indexada\n");
        printf("====================================\n");
        printf("1. Reconstruir os indices\n");
        printf("2. Mostrar produtos\n");
        printf("3. Mostrar acessos\n");
        printf("4. Pesquisar produto (busca binaria no arquivo)\n");
        printf("5. Pesquisar acesso (busca binaria no arquivo)\n");
        printf("6. Consultar produto pelo indice parcial\n");
        printf("7. Consultar acesso pelo indice parcial\n");
        printf("8. Inserir produto mantendo ordem fisica\n");
        printf("9. Remover produto logicamente\n");
        printf("10. Compactar arquivo (remover produtos excluidos)\n");
        printf("11. Mostrar produto mais caro\n");
        printf("12. Marcas com mais produtos\n");
        printf("0. Sair\n");
        printf("Opcao: ");

        if (scanf("%d", &opcao) != 1) {
            int c;
            printf("Entrada invalida.\n");
            while ((c = getchar()) != '\n' && c != EOF) { }
            opcao = -1;
            continue;
        }

        switch (opcao) {
            case 1:
                reconstruirIndiceProdutos();
                reconstruirIndiceAcessos();
                break;
            case 2: mostrarProdutos(); break;
            case 3: mostrarAcessos(); break;
            case 4: pesquisarProduto(); break;
            case 5: pesquisarAcesso(); break;
            case 6: consultarIndiceProduto(); break;
            case 7: consultarIndiceAcesso(); break;
            case 8: inserirProduto(); break;
            case 9: removerProduto(); break;
            case 10: compactarArquivoProdutos(); break;
            case 11: mostrarProdutoMaisCaro(); break;
            case 12: mostrarMarcasComMaisProdutos(); break;
            case 0: printf("Encerrando...\n"); break;
            default: printf("Opcao invalida.\n");
        }
    } while (opcao != 0);

    free(indicesProdutos);
    free(indicesAcessos);
    return 0;
}