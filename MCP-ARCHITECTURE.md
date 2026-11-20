# Serveur MCP natif pour nvc: architecture

Document de conception (aucun code écrit à ce stade). Il applique l'état de
l'art MCP à nvc et complète la section 4 de `PLAN.md`.

Sources lues le 15 novembre 2026, voir la section « Références ».

## 1. Ce que dit l'état de l'art, et ce qu'on en retient

### 1.1 Protocole: la révision 2026-07-28 change la donne

La révision courante de la spécification (2026-07-28) rompt avec la
précédente (2025-11-25):

- **Plus de session ni de handshake.** `initialize` et `Mcp-Session-Id`
  disparaissent. Chaque requête porte sa version et les capacités du client
  dans `_meta` (`io.modelcontextprotocol/protocolVersion`,
  `.../clientCapabilities`, `.../clientInfo` facultatif).
- **`server/discover` obligatoire**: versions supportées, capacités,
  identité, et `instructions` (guide d'usage destiné au LLM).
- **Tout résultat porte `resultType`** (`complete`, `input_required`, ou
  `task` avec l'extension tâches).
- **L'état entre appels passe par des handles explicites**: la spec le dit
  noir sur blanc pour les outils à état (`create_basket` puis
  `add_item(basket_id)`), avec des conseils sur l'entropie, l'opacité, la
  durée de vie annoncée dans la description de l'outil, et des erreurs
  d'expiration récupérables.
- **Les tâches longues sont une extension** (`io.modelcontextprotocol/tasks`):
  `tasks/get` en polling, `tasks/cancel` coopératif. Le support client varie.
- **Dépréciés, à ne pas adopter**: Roots, Sampling, Logging (on journalise
  sur `stderr`), HTTP+SSE.
- Le serveur ne peut pas émettre de requêtes vers le client sur stdio. Les
  interactions passent par `input_required` (MRTR).
- Un serveur **peut être bi-époque** (moderne et legacy dans le même
  processus): une requête avec `_meta` moderne est traitée sans état, un
  `initialize` sélectionne la sémantique legacy pour le processus stdio.

### 1.2 Conception des outils (guide d'ingénierie Anthropic)

- Peu d'outils, centrés sur des workflows, pas un wrapper par fonction.
  Préférer `search_*` à `list_*`.
- Préfixe de nommage commun (`nvc_…`), noms de paramètres non ambigus.
- Réponses à haut signal: noms lisibles plutôt qu'identifiants opaques,
  option `response_format` concis/détaillé (l'exemple Slack divise les
  jetons par ~3), pagination, filtres, troncature avec indication de comment
  affiner. Claude Code plafonne les réponses à 25 000 jetons.
- Erreurs actionnelles: dire quoi corriger et montrer un exemple valide.
- Descriptions écrites « pour un nouvel embauché ».
- Annotations MCP pour signaler les outils destructifs ou ouverts sur le
  monde.
- Évaluer sur des tâches réalistes à plusieurs étapes avec résultat
  vérifiable, lire les transcriptions complètes, et faire refactorer les
  outils par un agent avec un jeu de test réservé.

### 1.3 Sécurité (page « Security Best Practices » de MCP)

- Serveur local: transport stdio pour limiter l'accès au seul client,
  bac à sable aux privilèges minimaux, pas de commande arbitraire.
- Validation des entrées, contrôle d'accès, limitation de débit,
  assainissement des sorties (obligatoires côté serveur dans la page Tools).
- Handles d'état: aléatoires, non devinables, durée de vie bornée, ne jamais
  traiter la possession d'un handle comme une authentification.
- `$ref` de JSON Schema: ne jamais déréférencer un URI réseau.
- Les annotations d'outils sont non fiables pour un client tant que le
  serveur n'est pas de confiance.

### 1.4 Existant côté simulateurs

Les serveurs MCP de simulation que j'ai trouvés (Verilator, VUnit/GHDL,
cocotb, WEFT) sont des enveloppes qui lancent un processus de simulation par
exécution et renvoient un verdict, la fin du journal et le chemin de la
forme d'onde. Aucun ne garde un modèle de simulation vivant. Un serveur
natif de nvc peut offrir ce que ces enveloppes ne peuvent pas: interroger,
forcer et faire avancer une simulation en cours sans la relancer.

## 2. Décisions d'architecture

### D1. Deux processus: front-end de protocole et workers

**Contrainte constatée dans le code:** `fatal()`, `fatal_errno()` et
`fatal_trace()` (`src/util.c`) finissent par `fatal_exit()` qui appelle
`exit()` ou `_exit()`. `nvc.c` les utilise partout (bibliothèque absente,
unité introuvable, etc.). Un serveur à processus unique mourrait à la
première erreur fatale, et le modèle de simulation est lié au thread
(`__model`, `MODEL_ENTRY`), ce qui limite à une simulation par processus.

```
 client MCP ──stdio──▶ nvc --mcp  (front-end, petit, sans design chargé)
                         │  JSON-lines sur socketpair
            ┌────────────┼──────────────┐
            ▼            ▼              ▼
     worker éphémère  worker sim A   worker sim B
     (analyse/élab.)  (modèle vivant) (modèle vivant)
```

- Le front-end ne charge jamais de design: protocole, validation des
  entrées, limites, table des sessions, routage, journalisation.
- Un **worker éphémère** (`nvc --mcp-worker`, lancé par `exec` de
  `/proc/self/exe`) traite `nvc_analyse` et `nvc_elaborate`, puis sort.
- Un **worker de simulation** par `sim_id` détient le modèle (même chemin que
  `interact_cmd`: bibliothèque, `model_new`, `reheat`).
- Si un worker meurt (fatal, crash, OOM, timeout), le front-end répond par
  une erreur d'exécution d'outil actionnelle (« la simulation a été
  terminée: <raison>, rouvrez avec nvc_sim_open ») et la suite continue.
- Gains annexes: annulation par SIGKILL après un délai de grâce, `rlimit` et
  bac à sable appliqués au worker seulement, capture de sa sortie standard
  par un simple pipe, plusieurs simulations en parallèle.
- Les checkpoints du plan principal rendront les handles durables: un worker
  mort pourra être relancé depuis le dernier checkpoint.

### D2. Bi-époque: moderne (2026-07-28) et legacy (2025-11-25 et 2025-06-18)

Je n'ai pas pu vérifier quelles versions les clients courants parlent. Le
serveur sert donc les deux, sans coût de conception important:

- Première ligne avec `_meta.io.modelcontextprotocol/protocolVersion`:
  traitement sans état moderne. `server/discover` renvoie
  `supportedVersions`, `capabilities`, `serverInfo` et `instructions`.
- Première ligne `initialize`: répondre par la révision négociée,
  `notifications/initialized` accepté, `ping` accepté.
- Version non supportée: erreur `-32022` avec `data.supported`.
- `resultType` n'est émis que pour les requêtes modernes (un client legacy
  ignore ou traite l'absence comme `complete`).
- Une seule couche de logique d'outils. Seuls l'enveloppe et le décorateur de
  résultat diffèrent.
- Pas de Roots, Sampling, Logging ni HTTP+SSE.

### D3. Transport

- **stdio d'abord.** Un message JSON par ligne, sans retour à la ligne
  intégré (l'écrivain JSON échappe `\n`). Le serveur n'écrit **rien** d'autre
  que des messages MCP sur stdout et sort quand stdin atteint EOF.
- **stdout propre.** nvc écrit ses diagnostics et le `OUTPUT` de `std.textio`
  sur `stdout` (`nvc_stdout()`, `fileio.c`). Au démarrage le front-end ne
  lance que des workers dont stdout est un pipe capturé: aucune dérive
  possible vers le canal de protocole. `stderr` sert aux journaux, sans
  couleur (`--no-color`).
- **HTTP (Streamable) plus tard**, avec liaison à localhost seulement, vérification
  de `Origin` et jeton, conformément à la page de sécurité.

### D4. Simulation à état et handles explicites

- `nvc_sim_open` renvoie un `sim_id` opaque (128 bits aléatoires depuis
  `getrandom`). Sa durée de vie est écrite dans la description de l'outil
  (« expire après 30 min d'inactivité »).
- Un `sim_id` inconnu ou expiré donne `isError: true` avec la marche à
  suivre, pas une erreur de protocole.
- Les appels d'une même simulation sont sérialisés. Un second appel pendant
  un `nvc_sim_run` reçoit « occupé: simulation en cours depuis N ms,
  attendez ou annulez ».
- Limites par défaut: 4 simulations ouvertes, mémoire par worker bornée.

### D5. Appels longs: budget, progression, annulation

- `nvc_sim_run` exige toujours un budget: temps simulé (`for` ou `until`) et
  `max_wall_ms` (10 s par défaut). La réponse dit pourquoi la simulation
  s'est arrêtée (`time`, `wall_budget`, `assertion_failure`, `finished`,
  `cancelled`, `stop_on`). Le LLM relance avec `nvc_sim_run` jusqu'au résultat
  voulu. C'est universel (aucun support client spécial) et reprenable.
- **Progression:** si la requête contient `_meta.progressToken`, le worker
  émet `notifications/progress` au plus toutes les 250 ms, `progress` croissant
  (femtosecondes simulées), `total` si on connaît le temps final.
- **Annulation:** sur stdio, le client envoie `notifications/cancelled`. Un
  thread lecteur du front-end la reçoit même pendant un appel long, puis
  demande au worker d'arrêter (`model_interrupt`, qui existe déjà et sert au
  SIGINT) et, sans réponse en 2 s, le tue. Le serveur n'envoie plus rien pour
  la requête annulée.
- **Extension tâches:** v2, une fois qu'on sait quels clients l'annoncent.
  `nvc_sim_run` pourra alors renvoyer `resultType: "task"` pour les longs runs.

### D6. Surface d'outils (14 au plus, préfixe `nvc_`)

Un outil par intention. Pas de drapeau libre en ligne de commande: seulement
des options typées (liste fermée), pour éviter l'injection d'options.

| Outil | Rôle | readOnly | destructive | idempotent |
|---|---|---|---|---|
| `nvc_analyse` | Analyser des fichiers VHDL/SV, renvoyer les diagnostics | non | non | oui |
| `nvc_elaborate` | Élaborer un top-level avec generics | non | non | oui |
| `nvc_sim_open` | Ouvrir une simulation, renvoyer `sim_id` | non | non | non |
| `nvc_sim_run` | Avancer avec budget et conditions d'arrêt | non | non | non |
| `nvc_sim_close` | Fermer une simulation | non | non | oui |
| `nvc_design_search` | Chercher signaux/régions par motif, paginé | oui | non | oui |
| `nvc_signal_get` | Lire des signaux (jusqu'à 64), radix choisi | oui | non | oui |
| `nvc_signal_set` | `force` / `deposit` / `release` | non | non | oui |
| `nvc_wave_start` | Enregistrer des signaux (mémoire, fichier FST, ou les deux) | non | non | oui |
| `nvc_wave_get` | Lire les changements sur une fenêtre (mémoire ou fichier) | oui | non | oui |
| `nvc_coverage_report` | Résumé/éléments non couverts d'un `.ncdb` | oui | non | oui |
| `nvc_sim_checkpoint` | Plus tard: sauvegarder l'état | non | non | non |
| `nvc_sim_restore` | Plus tard: reprendre d'un checkpoint | non | non | non |

Tous ont `openWorldHint: false` (système fermé). `nvc_sim_close` n'est pas
marqué destructif: il ne supprime que l'état de simulation en mémoire.

`nvc_trace_*` répond directement à « signal oublié »: on enregistre en mémoire
à la demande, en attendant les checkpoints qui permettront de rejouer.

**Règles de réponse:**

- `structuredContent` conforme à un `outputSchema`, et le même JSON
  sérialisé dans un bloc `text` (la spec l'impose en SHOULD pour la
  compatibilité). Chaque réponse reste petite par construction: `limit` par
  défaut (50), `truncated: true` plus un conseil pour affiner.
- `response_format: "concise" | "detailed"` (concis par défaut): le détaillé
  ajoute positions source, identifiants et astuces.
- Valeurs de signaux lisibles (`'1'`, `x"3F"`, `RUNNING`), pas des octets
  bruts. Chemins hiérarchiques complets (`/top/dut/fifo/count`).
- **Une analyse qui trouve des erreurs VHDL n'est pas une erreur d'outil**:
  c'est un résultat `ok: false` avec `diagnostics[]`. `isError: true` est
  réservé aux mauvaises utilisations (handle expiré, signal inconnu, valeur
  mal formée) avec correction proposée, par exemple « signal '/top/clk2'
  introuvable. Vouliez-vous '/top/clk'? Utilisez nvc_design_search ».
- Les erreurs de protocole (`-32602`, outil inconnu, requête malformée) ne
  servent qu'à la structure de la requête.
- `tools/list` renvoie les outils **dans un ordre déterministe** avec
  `ttlMs` et `cacheScope: "public"`.

**Diagnostics structurés:** hook existant `diag_set_consumer` (utilisé par
les tests unitaires) avec les accesseurs `diag_get_text`, `diag_get_hint`,
`diag_get_trace`, `diag_get_loc`, `diag_level`. Aucun refactor du cœur pour
v1. Objet émis:
`{severity, file, line, col, end_line, end_col, message, hints[]}`.

### D7. Sécurité

Le serveur exécute du code sur la machine de l'utilisateur et il lit du VHDL
non fiable.

- **Racine de travail.** Tous les chemins sont résolus avec `realpath` puis
  vérifiés contre la racine (`--mcp-root`, défaut: répertoire courant).
  Échappement par `..` ou lien symbolique refusé.
- **Pas de TCL arbitraire, pas de shell.** Aucun outil n'expose `exec`,
  `source` ni une ligne de commande libre.
- **Code natif désactivé par défaut.** Le chargement de plugins VHPI/VPI et
  de bibliothèques `VHPIDIRECT` (`NVC_PLUGIN_PATH`) exige
  `--mcp-allow-plugins`, car c'est de l'exécution de code arbitraire.
- **E/S fichier du design.** `std.textio` et `file` ouvrent des fichiers avec
  les droits du serveur. On ajoute un filtre de chemin dans `fileio.c`
  (même racine) pour les workers MCP, et on recommande un bac à sable OS
  (bwrap, Landlock ou conteneur) pour les environnements partagés.
- **Limites par worker:** `RLIMIT_AS`, `RLIMIT_CPU`, `RLIMIT_NOFILE`, taille
  des fichiers, `max_wall_ms` par appel, nombre d'appels par seconde
  (limitation de débit exigée par la spec), taille maximale de requête
  (1 MiB) et de réponse (par défaut ~25 000 jetons).
- **Analyseur JSON durci:** profondeur maximale, taille maximale, UTF-8
  validé, clés dupliquées refusées, jamais de récursion non bornée.
  À fuzzer avant la première version.
- **Sorties non fiables.** Messages `report`, noms d'objets du design et
  fichiers lus sont du contenu hostile potentiel (injection de prompt).
  Ils vont dans des champs dédiés (`sim_output`, `messages[]`) et les
  `instructions` de `server/discover` disent au modèle de les traiter comme
  des données, jamais comme des instructions.
- **Handles:** 128 bits aléatoires, expiration inactive, jamais enregistrés
  sur disque. En HTTP, ils seront liés au principal authentifié.

### D8. Organisation du code

Nouveau répertoire `src/mcp/`, sans dépendance externe, compilé par défaut
(`--disable-mcp` pour l'exclure):

| Fichier | Contenu |
|---|---|
| `mcp-json.c/h` | Analyseur et écrivain JSON bornés, allocation par arène |
| `mcp-proto.c/h` | JSON-RPC, détection d'époque, `server/discover`, `initialize` legacy, `tools/list`, `tools/call`, annulation, progression |
| `mcp-tools.c/h` | Registre déclaratif: nom, titre, description, schéma d'entrée, schéma de sortie, annotations, gestionnaire |
| `mcp-schema.c/h` | Validateur minimal (type, required, enum, min/max, `additionalProperties:false`) |
| `mcp-supervisor.c` | Table des sessions, lancement et surveillance des workers, délais, tuerie |
| `mcp-worker.c` | Boucle du worker: analyse, élaboration, simulation, via les API existantes |
| `mcp-diag.c` | Conversion `diag_t` en JSON |

Les schémas sont **la source unique**: le validateur d'entrée, `tools/list` et
la documentation sont tous générés depuis la même structure déclarative, pour
éviter la dérive.

Intégration à `nvc.c`: deux nouvelles commandes (`--mcp`, `--mcp-worker`) sur
le modèle de `interact_cmd`. Les fonctions `analyse()` et `elaborate()` de
`nvc.c` mélangent aujourd'hui analyse d'`argv` et travail: v1 appelle le
chemin existant en construisant un `argv` synthétique à partir d'options
typées, puis on factorisera si le besoin se confirme.

## 3. Tests et évaluation

1. **Tests unitaires** (`test/test_mcp.c`): JSON (cas limites, profondeur,
   UTF-8), validateur de schéma, dispatch, mapping d'erreurs.
2. **Transcriptions d'or:** fichiers JSON-lines d'entrée et de sortie
   attendue (`test/mcp/`) rejoués contre `nvc --mcp`, couvrant les deux
   époques, `UnsupportedProtocolVersion`, annulation, worker tué, handle
   expiré, troncature.
3. **Fuzzing** de l'analyseur JSON et de la couche de protocole.
4. **Conformité** avec un client MCP de référence (inspecteur officiel).
5. **Évaluation façon Anthropic:** 10 à 20 tâches réalistes en plusieurs
   étapes avec résultat vérifiable (ex.: « trouver pourquoi l'assertion de
   `fifo_tb` échoue et donner le temps et le signal fautif »), suivi de la
   précision, du temps, du nombre d'appels, des jetons et des erreurs
   d'outils. Lire les transcriptions, ajuster les descriptions, vérifier
   sur un jeu réservé.

## 4. Feuille de route

| Étape | Contenu | Critère de sortie |
|---|---|---|
| M0 | JSON, boucle stdio, bi-époque, `server/discover`, `tools/list`, `nvc_analyse` via worker éphémère | Transcriptions d'or des deux époques; un fatal ne tue pas le serveur |
| M1 | `nvc_elaborate`, `nvc_sim_open/run/close`, `nvc_design_search`, `nvc_signal_get/set`, budgets, progression, annulation | Scénario complet analyser, simuler, inspecter, corriger, relancer |
| M2 | `nvc_trace_*`, diagnostics enrichis, `response_format` partout | Tâches d'évaluation au-dessus d'un seuil à fixer |
| M3 | Bac à sable, filtre de chemins `fileio`, rlimits, fuzzing, `--disable-mcp` | Revue de sécurité sans point bloquant |
| M4 | Checkpoints (`PLAN.md` §1), extension tâches, HTTP local, couverture | Handles qui survivent à la mort d'un worker |

## 5. État d'avancement (M0, M1 et M2)

L'étape M0 est écrite (`src/mcp/`, tests dans `test/test_mcp.c`):

- JSON borné, validateur de schémas, protocole bi-époque, annulation,
  limitation de la taille des requêtes, outil `nvc_analyse`.
- Écart par rapport à D1: le worker éphémère est le binaire `nvc` lui-même
  (`nvc [options globales] --work=… -a …`) lancé par `fork`/`exec`. Les
  diagnostics reviennent en JSON sur un descripteur dédié grâce à l'option
  globale interne `--diag-json=FD`, et la sortie texte (erreurs fatales) est
  capturée à part. La commande `--mcp-worker` persistante n'apparaîtra qu'avec
  les simulations (M1).
- Le fuzzing de l'analyseur JSON (4 millions d'entrées mutées sous ASan et
  UBSan) n'a rien trouvé. Le harnais n'est pas versionné.
- Pas encore fait: l'option `--disable-mcp` à la configuration, les
  sorties de progression, les limites mémoire (`RLIMIT_AS`) des workers,
  et le support Windows.

### M1: simulations vivantes et outils de listing

Quatorze outils au total: `nvc_analyse`, `nvc_elaborate`, `nvc_workspace_list`,
`nvc_library_list`, `nvc_sim_open`, `nvc_sim_run`, `nvc_sim_close`,
`nvc_design_list`, `nvc_design_search`, `nvc_signal_get`, `nvc_signal_set`,
`nvc_trace_start`, `nvc_trace_get`, `nvc_trace_stop`.
Choix de réalisation, avec ce qui les a motivés:

- **Un processus par simulation** (`nvc --mcp-worker --fd=3 TOP`). Le
  superviseur lui parle en JSON par lignes sur un socket dont le numéro est
  fixé à 3 (un shell comme dash ne sait pas rediriger au-delà de 9, ce qui
  rend le contrat testable avec de faux workers). Le worker part avec
  `PR_SET_PDEATHSIG`, dans son propre groupe de processus, et est tué avec le
  serveur.
- **Exécution par tranches.** `model_run` rejoue les callbacks de début à
  chaque appel et refuse de reprendre après une interruption, donc le modèle
  expose `model_start`, `model_run_cycles` et `model_end`. Le worker lit
  l'horloge, l'annulation et la progression entre deux tranches (de taille
  adaptative, visée à 20 ms). Un drapeau de halte est testé après chaque
  cycle, ce qui permet de s'arrêter exactement au cycle où un message de la
  sévérité demandée apparaît (65 ns, pas 140 ns).
- **Budget de temps réel.** `max_wall_ms` (10 s par défaut) borne chaque
  appel. Un worker muet (un processus sans `wait`) est tué après un délai de
  grâce de 3 s et le handle disparaît.
- **Messages structurés.** Les `report` et `assert` passent par le hook de
  diagnostics et reviennent avec le temps simulé, le delta et la trace
  (100 au plus, les compteurs restent exacts). Ce que le design imprime sur
  stdout est capturé dans un fichier mémoire et relu à chaque appel, ce qui
  évite tout problème d'ordre entre deux canaux.
- **Relatif et absolu.** `duration` se compte depuis la fin du run précédent,
  pas depuis le dernier événement: deux runs de 100 ns avancent de 200 ns.
- **Valeurs compactes.** Un vecteur de `std_logic` sort en `0101`, ou en
  `0x5` / `5` avec `radix`, au lieu de `('0', '1', '0', '1')`.
- **Pas de `deposit`.** `deposit_signal` suppose un processus actif et ne
  peut pas être appelé de l'extérieur: seuls `force` et `release` existent,
  comme dans le shell TCL.
- **Glob par `fnmatch`.** `ident_glob` de nvc refuse qu'un `*` final soit
  vide et n'a pas de `?`: la recherche utilise `fnmatch` insensible à la
  casse.
- **Aucun effet de bord à la lecture.** `nvc --list` crée la bibliothèque si
  elle n'existe pas: `nvc_library_list` vérifie d'abord son existence.

### M2: traces et simulations longues

`nvc_trace_start` enregistre en mémoire les changements de signaux choisis
(chemins ou motif, 64 au plus, 256 octets par valeur), `nvc_trace_get` les
relit par fenêtre de temps, paginés, et `nvc_trace_stop` libère la mémoire.
C'est la réponse au problème des signaux oubliés dans les ondes: on regarde
en arrière au lieu de relancer. Les valeurs sont celles de la fin de chaque
pas de temps, comme dans un fichier d'ondes (observateurs au niveau
« postponed »), regroupées par instant pour limiter le volume. Un tampon
circulaire garde les `max_changes` plus récents (10 000 par défaut, 500 000
au plus, 64 Mo au plus): `dropped` et `oldest_time_fs` disent ce qui a été
perdu.

**Ce que les mesures montrent** (build de debug, 16 compteurs de 32 bits,
4 cœurs; la référence est `nvc -r` sur la même simulation):

| Mesure | Résultat |
|---|---|
| 5 ms simulées, `nvc -r` | 3,58 s |
| 5 ms, un seul `nvc_sim_run` | 3,52 s (pas de surcoût mesurable) |
| 5 ms en 10 appels de 0,5 ms | 3,80 s (+8 %) |
| 5 ms avec budget de 1 s par appel | 4 appels, 3,81 s, reprise exacte |
| 5 ms avec une trace sur l'horloge (1 M de changements) | 3,86 s (+10 %) |
| 1 ms avec 16 signaux de 32 bits tracés | 0,81 s contre 0,77 s (+5 %) |
| Mémoire du worker | 13 Mo au repos, 35 Mo avec un tampon de 500 000 changements, stable ensuite |
| Annulation d'un run lourd | la requête suivante est servie en 1 ms, l'état est conservé |

**Ce qui est possible à ce stade pour une simulation longue:** la faire
avancer par tranches de temps réel (`max_wall_ms`) avec reprise exacte,
suivre sa progression, l'annuler sans la perdre, l'arrêter au premier
message d'erreur, inspecter et forcer des signaux, regarder en arrière avec
une trace, et en garder plusieurs ouvertes (4 au plus).

**Ce qui ne l'est pas encore:**

- *Rien ne survit à la mort du worker, du serveur ou du client.* Une
  simulation de plusieurs heures est perdue si le processus meurt (manque de
  mémoire, arrêt du client MCP, redémarrage du serveur) ou si elle reste 30
  minutes sans appel. Il faut les checkpoints (`PLAN.md` §1).
- *Pas de retour en arrière.* La trace relit le passé des signaux qu'on a
  choisis avant de simuler, mais on ne peut pas revenir à un instant antérieur
  pour changer ce qu'on observe ou corriger le code: même cause, mêmes
  checkpoints.
- *Pas de saut des périodes calmes* (liaisons série lentes): le temps de
  calcul reste proportionnel au temps simulé.
- *Pas de tâches asynchrones au sens MCP.* Chaque appel garde la connexion
  jusqu'à `max_wall_ms` (300 s au plus). Les clients ont leurs propres
  délais: viser 30 à 60 s par appel et laisser la progression et la reprise
  faire le reste. Le comportement des clients n'a pas été testé.
- *Les messages sont plafonnés* à 100 par appel (les compteurs restent
  exacts), et la sortie texte à 16 Ko.

### Évaluation avec un agent (Claude Agent SDK)

`test/mcp/agent_eval.py` fait piloter le serveur par un vrai agent (modèle
`claude-haiku-5-5`, outils limités à `Read`, `Edit`, `Glob` et `nvc_analyse`,
sans `Bash` pour que l'agent ne puisse pas contourner le serveur). Le
résultat est vérifié en relançant nvc directement, pas sur la foi de
l'agent. Ce n'est pas dans `make check`: il appelle un modèle.

| Scénario | Résultat |
|---|---|
| Corriger une faute de frappe dans un design de deux fichiers | 7 tours, 2 appels MCP, corrigé et reconfirmé |
| Compiler dans le désordre avec `--std 2008` | 1 appel: l'agent a réordonné lui-même et passé `std: "2008"` |
| Chemin faux, sans aucun outil de fichiers | voir ci-dessous |
| Découvrir les entités d'un espace de travail inconnu (aucun outil de fichiers) | 5 appels MCP, les deux entités |
| Trouver quand une assertion échoue et la valeur du registre (aucun outil de fichiers) | 9 appels, `65 ns 5`, exact |
| Relire les valeurs d'un signal avant l'échec, un seul run autorisé (aucun outil de fichiers) | 9 appels: trace démarrée avant le run, puis relue, `3, 4, 5` |

**Leçon:** au premier essai, le dernier scénario échouait. Le message
disait seulement « fichier introuvable » avec la racine, l'agent n'avait
aucun moyen de découvrir `src/counter.vhd` et a abandonné (en le disant
honnêtement). Le message propose maintenant les fichiers de même nom
trouvés sous la racine (« Did you mean: src/counter.vhd? »), ou à défaut
le contenu de la racine. Le même scénario réussit alors en deux appels.
Un agent qui n'a que ce serveur doit pouvoir se rattraper avec ce que le
serveur lui dit.

**Deuxième leçon (M1):** au premier essai du scénario de débogage,
l'agent a demandé `pause_on: "failure"` (« l'assertion échoue » devenait la
sévérité *failure*), donc la simulation a couru 10 s sans s'arrêter avant
qu'il ne la rouvre. Sa recherche `cnt*` ne trouvait rien à cause de
`ident_glob`. Corrections: description de `pause_on` réécrite autour de ce
que l'on cherche, recherche par `fnmatch`, et un champ `hint` dans les
résultats de `nvc_sim_run` qui dit quoi faire ensuite (par exemple « le
design a signalé beaucoup d'erreurs: utilisez pause_on error »). Même
scénario ensuite: 9 appels au lieu de 15, sans faux pas.

Limites: un petit modèle, six scénarios courts, un seul essai chacun.
C'est un test de fumée, pas une mesure statistique.

## 6. Points à valider et limites de cette étude

À décider avec vous:

1. **Clients visés.** Si vous n'utilisez que des clients récents, on peut
   supprimer le mode legacy et gagner en simplicité. Je n'ai pas pu
   vérifier la version parlée par chaque client.
2. **Un worker par simulation** (D1) est la décision la plus structurante.
   L'alternative en un seul processus est plus simple mais fragile face à
   `fatal()`.
3. **Plugins natifs désactivés par défaut** (D7): acceptable pour vos usages
   (VHPI, `VHPIDIRECT`)?

Incertitudes de ma part:

- Les noms de champs des annotations (`readOnlyHint`, `destructiveHint`,
  `idempotentHint`, `openWorldHint`) viennent de révisions antérieures de la
  spec. Je n'ai pas relu la définition exacte dans le schéma de la
  révision 2026-07-28 (la page du schéma est tronquée).
- Je n'ai pas vérifié que `model_interrupt` peut être appelé depuis un autre
  thread que celui du modèle. Dans le plan, il est appelé par le worker
  lui-même, sur demande du front-end par le socket.
- La concurrence sur la bibliothèque de travail (plusieurs workers
  écrivant dans le même `work/`) n'a pas été étudiée.
- Le comportement sous Windows n'a pas été regardé (socketpair, `exec` de
  `/proc/self/exe`).

## 7. Processus longs et LLM: état de l'art

Question: comment le LLM voit l'état d'une simulation longue et en
apprend la fin sans interroger en boucle? Lectures du 19 novembre 2026,
sources en fin de document. Rien de ce qui suit n'a été testé avec un vrai
client MCP.

### 7.1 Ce que dit le protocole

| Mécanisme | Ce qu'il apporte | Limites |
|---|---|---|
| `notifications/progress` | Progression d'un appel en cours, avec un message lisible | Liée à une requête en cours, s'arrête à sa fin. La spécification ne dit pas si le modèle la voit: c'est laissé à l'application hôte |
| Extension tâches (`io.modelcontextprotocol/tasks`, 2026-07-28) | Le serveur répond `resultType: "task"` avec un `taskId`, le client interroge `tasks/get`, annule par `tasks/cancel`. `statusMessage` (peut être montré à l'utilisateur ou au modèle) décrit l'avancement. `notifications/tasks` via `subscriptions/listen` | Interrogation pure, sans attente longue ni délai (`pollIntervalMs` seulement). Pas de `tasks/list`. La survie à un redémarrage du serveur n'est pas définie. Aucune notification de progression sur une tâche: seul `statusMessage` l'exprime |
| `subscriptions/listen` | Pousse `list_changed`, `resources/updated` et les tâches suivies | La spécification ne dit pas que le modèle en est informé |

Le serveur décide pour chaque requête s'il répond par une tâche, mais
seulement si le client a déclaré l'extension; une erreur tool reste un
résultat `completed` avec `isError: true`.

### 7.2 Ce que font les clients (Claude Code, d'après sa documentation)

- **Tâches MCP en bêta publique** depuis Claude Code 2.1.304: l'appel rend la
  main quand le serveur accepte le travail, une relève toutes les 2 s par
  défaut (bornée à 250-30 000 ms), un panneau `/tasks`, annulation depuis ce
  panneau. Pas dans VS Code ni l'application de bureau. Une tâche d'un
  serveur stdio s'arrête avec le processus de Claude Code. La matrice de
  clients de modelcontextprotocol.io a une colonne Tasks vide pour tous les
  clients: elle n'est pas à jour ou non définie, ne pas la lire comme un non.
- **Mise en arrière-plan automatique d'un appel MCP ordinaire**: un appel
  encore en cours après 2 minutes (`CLAUDE_CODE_MCP_AUTO_BACKGROUND_MS`, 0
  pour désactiver) devient une tâche de fond; le modèle reçoit un
  identifiant, continue de travailler et reçoit le résultat comme
  notification. C'est, aujourd'hui et sans rien changer au serveur, le seul
  mécanisme documenté qui réveille le modèle à la fin d'un appel long. Il ne
  vaut pas pour les sous-agents, les exécutions non interactives, ni les
  appels qui attendent une saisie.
- **Délais**: limite par appel `timeout` du serveur (dure, la progression ne
  la repousse pas), `MCP_TOOL_TIMEOUT` (environ 28 h si absent), délai
  d'inactivité de 30 min en stdio (la progression compte comme activité).
- **Sortie**: 25 000 jetons par défaut (`MAX_MCP_OUTPUT_TOKENS`), au-delà le
  résultat est écrit dans un fichier.
- **Canaux (« channels »)**, aperçu de recherche: un serveur MCP pousse
  `notifications/claude/channel` dans la session, le modèle le reçoit comme
  une balise `<channel>`, et le même serveur peut exposer des outils. Il faut
  `--channels`, une liste d'autorisation (un canal personnalisé exige
  `--dangerously-load-development-channels`, interactif seulement, ignoré
  avec `-p` et le SDK), une authentification claude.ai ou Console, et la
  politique de l'organisation. Des rapports signalent des événements mis en
  tampon plutôt que réveillant une session inactive. Un serveur qui négocie
  la révision 2026-07-28 n'est pas enregistré comme canal (il faut le forcer
  en négociation ancienne). Propre à Claude Code et instable.

### 7.3 Ce que font les agents en général

Le schéma dominant: l'outil rend une **poignée tout de suite**, le travail
continue, et le résultat arrive par **interrogation, source de vérité**, avec
la **poussée en optimisation**. Quand on pousse, l'événement est injecté dans
le contexte de l'agent pour le réveiller. Les mises en garde reviennent:
idempotence des relances, état durable après un plantage, et le lien entre
l'événement et l'appel d'origine à rétablir soi-même.

### 7.4 Ce que cela implique pour nvc

Aujourd'hui `nvc_sim_run` est un appel bloquant et borné: il marche partout
mais le LLM ne voit rien pendant qu'il attend, et une simulation ne peut pas
avancer entre deux appels. Conception proposée, par couches, la première
suffisant à tous les clients:

1. **Un run de fond côté worker** (portable). `nvc_sim_run` accepte
   `background: true` et rend tout de suite; le worker continue et, entre deux
   tranches, répond à des requêtes en lecture. Nouveaux outils:
   - `nvc_sim_status`: ne bloque jamais. État (`running`, `paused`,
     `finished`, `terminated`), temps simulé, temps réel écoulé, débit (fs
     simulées par seconde réelle) et temps restant estimé si une cible existe,
     compteurs de messages, derniers messages.
   - `nvc_sim_wait`: attente longue bornée (30 s au plus) qui rend la main à
     la fin du run, au premier message de la sévérité demandée ou après le
     délai. Une seule requête remplace une boucle d'interrogation, sans
     jetons entre deux.
   - Un **journal d'événements numérotés** (`since_seq`): rien n'est perdu si
     le LLM interroge tard, ce qui rend l'interrogation fiable et sans doublon.
   - `nvc_signal_get`, `nvc_design_*`, `nvc_trace_get` et `nvc_signal_set`
     restent utilisables pendant un run, et leur résultat porte l'état courant.
2. **Tâches MCP** pour les clients qui les déclarent: `nvc_sim_run` en fond
   répond `resultType: "task"`, avec `statusMessage` du genre « 3,2 ms sur
   10 ms, 2 erreurs, dernière: counter reached five », ce qui rend l'état
   visible sans outil supplémentaire. `tasks/cancel` arrête le run, et
   `notifications/tasks` complète la relève. À prévoir: la tâche vit et
   meurt avec le worker.
3. **Mise en arrière-plan de Claude Code**: un `nvc_sim_wait` plus long que
   le seuil de 2 minutes devient une tâche de fond et son résultat réveille le
   modèle. À vérifier avant de compter dessus.
4. **Canaux**, en dernier et facultatif: seulement si l'essai montre que les
   couches précédentes ne suffisent pas.

La survie d'un run à la mort du worker, du serveur ou du client reste
l'affaire des checkpoints (`PLAN.md` §1): ni les tâches MCP (processus stdio)
ni le run de fond n'y répondent.

## 8. Gros fichiers d'une simulation longue

### 8.1 Ce qui grossit, mesuré

Build de debug, 4 cœurs. Deux designs de 16 registres de 32 bits à 100 MHz:
des compteurs (données très régulières) et des registres pseudo-aléatoires
(cas défavorable).

| Fichier | Mesure | Croît avec |
|---|---|---|
| FST, compteurs, tous les signaux | 12 Ko pour 2 ms (6,8 millions de changements relus) | le temps, mais se compresse presque à rien |
| VCD, mêmes compteurs | 227 Mo pour 2 ms | le temps, sans compression |
| FST, registres aléatoires, tous les signaux | 7,2 Mo par ms simulée, soit environ 7 Go par seconde simulée | le temps, le nombre de signaux et leur activité |
| FST, un seul registre aléatoire | 0,45 Mo par ms, 16 fois moins | proportionnel au nombre de signaux actifs |
| Surcoût de calcul d'un FST complet | +15 % (11,3 s contre 9,8 s); filtré: indiscernable | |
| Journaux (`report`, `textio`) | non borné par nvc | l'activité du design |
| Fichier de couverture | borné par la taille du design | pas le temps |
| Checkpoints (à venir) | taille de l'état, pas du temps | le nombre d'instantanés gardés |

La compression du FST dépend entièrement de la régularité des données: les
compteurs sont un cas extrême, pas une prévision. La syntaxe des filtres de
nvc utilise `:` comme séparateur (`--include=':top:gen(0):u:r'`), pas `/`.

### 8.2 État actuel du serveur

Aucun fichier d'ondes n'est produit par le serveur MCP. Ce qui existe: les
traces en mémoire (bornées, 64 Mo au plus), `sim_output` plafonné à 16 Ko par
appel, 100 messages par appel (compteurs exacts), et un fichier en mémoire
pour la sortie texte qui n'est borné que par `RLIMIT_FSIZE` (256 Mo): au-delà,
les écritures échouent sans message. Ce dernier point est un défaut à
corriger. Le même plafond couperait un fichier d'ondes écrit par le worker:
il faudra le traiter à part.

### 8.3 Principes proposés

1. **Le LLM ne lit jamais un gros fichier, il l'interroge.** Claude Code
   écrit déjà dans un fichier tout résultat de plus de 25 000 jetons: un
   résultat qui déborde est perdu pour le raisonnement. Les outils rendent
   donc des réponses de taille bornée, paginées, et les gros volumes restent
   sur disque derrière un outil de requête.
2. **Les fichiers vivent dans l'espace de travail** (`.nvc-mcp/<sim_id>/`),
   sous la même confinement que les sources, visibles de l'utilisateur et des
   autres outils. Les résultats rendent le chemin, la taille, la plage de
   temps et le débit de croissance, jamais le contenu (le type
   `resource_link` de MCP convient à un chemin).
3. **Le disque est borné et la limite est explicite.** Quota par simulation
   et global. Au quota: soit l'enregistrement s'arrête avec un message qui
   le dit, soit des **segments tournants** (un FST ne se tronque pas: un
   nouveau fichier toutes les N Mo ou tous les T de temps simulé, en gardant
   les K derniers). Une simulation ne remplit jamais le disque en silence.
4. **Enregistrer peu par défaut.** Le coût et la taille dépendent du nombre
   de signaux actifs. Ondes désactivées par défaut, activées à l'ouverture
   avec des filtres (`include`/`exclude`, et éventuellement une fenêtre de
   temps), et l'état annonce la taille et le débit avant qu'elle ne soit un
   problème.
5. **Interroger le fichier côté serveur.** La bibliothèque FST de `thirdparty`
   contient déjà le lecteur (`fstReaderOpen`, `fstReaderIterBlocks2`,
   `fstReaderSetFacProcessMask`), qui ne traite que les signaux demandés:
   - `nvc_wave_summary`: signaux présents, plage de temps, nombre de
     changements par signal;
   - `nvc_wave_get`: une fenêtre de temps pour quelques signaux, au même
     format que `nvc_trace_get`;
   - `nvc_wave_find`: premier instant où un signal prend une valeur, ou
     premier front après un instant donné.
   Le coût est celui des signaux demandés, pas de la taille du fichier.
6. **Les journaux suivent la même règle**: un fichier persistant avec un
   outil de lecture par décalage et de recherche, les compteurs restant
   exacts dans les résultats.
7. **Rétention**: durée de vie des fichiers, option pour les garder à la
   fermeture, nettoyage limité au répertoire `.nvc-mcp/`. Les checkpoints
   partageront répertoire et quotas.

### 8.4 Ordre de réalisation

1. Corriger le plafond silencieux de la sortie texte.
2. Ondes à l'ouverture (`wave` avec filtres, quota, segments) via le dumper de
   nvc, et taille/débit dans `nvc_sim_status`.
3. `nvc_wave_summary`, `nvc_wave_get`, `nvc_wave_find` par le lecteur FST.
4. Journal persistant et sa lecture.
5. Rétention et nettoyage.

### 8.5 Réalisé

Étapes 1 à 3 de §8.4 et le run de fond de §7.4:

- **Run de fond.** `nvc_sim_run` accepte `background:true` (sans limite de
  temps réel, aucune limite); le worker avance alors le run par tranches entre
  deux requêtes et répond aux lectures (`list`, `search`, `get`, `trace_get`,
  `status`, `wave_start`, `wave_stop`) pendant qu'il tourne. `nvc_sim_status`
  donne l'état (`running`, `idle`, `finished`, `terminated`), le temps simulé,
  le débit (fs simulées par seconde réelle), le pourcentage et l'ETA quand
  une cible existe, les messages depuis `since_seq` (numéros croissants) et
  les tailles de fichiers. `nvc_sim_wait` (≤ 30 s) sonde le worker toutes les
  250 ms côté serveur et rend la main à l'arrêt du run, à l'apparition de
  nouvelles erreurs, à l'annulation ou au délai. `nvc_sim_stop` termine le
  run sans fermer la simulation. Un run de fond n'a aucune limite de durée et
  n'est jamais expiré (il peut durer des jours; le front-end étant lié à son
  client, les workers disparaissent avec lui).
- **Vivant ou planté ?** `nvc_sim_status` donne `advanced_since_last_status_fs`
  (le temps simulé doit avancer), `worker.cpu_seconds`, `memory_mb` et
  `os_state` (lus dans `/proc`). Si le worker est occupé dans un seul pas long
  d'un process du design et ne répond pas sous 3 s, l'appel ne échoue pas:
  il rend `responsive:false`, la dernière heure connue et un conseil (CPU qui
  croît = travaille, plat ou état Z/T = bloqué); `nvc_sim_wait` rend alors
  la main avec `woke_because:unresponsive`. La réponse tardive est écartée.
- **Fichiers.** Chaque simulation a un répertoire `.nvc-mcp/<sim_id>/` dans
  l'espace de travail, jamais supprimé automatiquement. La sortie du design
  y est écrite (`output.log`, plafond 1 Gio puis abandon avec un message:
  le plafond de 256 Mio, silencieux, a disparu). `nvc_wave_start` démarre
  un fichier FST à tout moment, avec `include`/`exclude` (chemins du design,
  `*` joker) et un quota `max_bytes` (1 Gio par défaut); au quota,
  l'enregistrement s'arrête avec un message et la simulation continue.
  `nvc_wave_stop` finalise le fichier, ce qui permet d'en commencer un autre
  (segments). `nvc_sim_close` attend jusqu'à 3 s que le worker finalise ses
  fichiers; un worker tué avant laisse un FST illisible (le message d'erreur
  le dit).
- **Lecture.** `nvc_wave_summary`, `nvc_wave_get` (fenêtre, `max_changes`,
  radix) et `nvc_wave_find` lisent un fichier terminé par le lecteur FST
  dans le front-end, sans simulation vivante; `nvc_sim_log` lit une plage,
  la fin ou les lignes contenant un motif. Seuls les fichiers sous
  `.nvc-mcp/` sont lisibles (`realpath`).

- **Lecture en direct.** Le tampon en mémoire est celui des traces:
  `nvc_trace_start` et `nvc_trace_stop` sont acceptés pendant un run de fond
  (entre deux tranches) et `nvc_trace_get` lit les derniers changements
  jusqu'à l'instant présent, sans rien arrêter. Jusqu'à 512 signaux par
  trace, 4 traces, 64 Mio. À la différence d'un FST, cela ne demande aucune
  fermeture.

Limites connues: un FST en cours d'écriture n'est pas lisible (il faut
`nvc_wave_stop`); l'enregistrement démarré en cours de run ne couvre que
l'intervalle à partir du démarrage; `nvc_wave_find` compare des valeurs
binaires (les énumérations multi-bits se donnent telles qu'écrites dans le
fichier); la rétention automatique des répertoires (étape 5) reste à faire;
les tâches MCP (extension `tasks`) ne sont pas encore proposées.

### 8.6 Diagnostic par les ondes (voir `MCP-DEBUG-PLAN.md`)

Réalisé: `nvc_trace_*` a été fusionné dans `nvc_wave_*` (un seul
enregistrement, `to: memory|file|both`, lecture par `sim_id` ou `file`),
un petit langage de conditions (`mcp-cond.c`: `/dut/valid = '1' and
/dut/ready = '0'`, `rose()`, `fell()`, `changed()`, `stable()`),
`nvc_wave_find` (stretches vraies, par front d'horloge, `at_least_cycles`),
`nvc_wave_measure` (ordre et délais entre deux événements, en temps et en
cycles), `nvc_wave_table` (une ligne par front d'horloge, lignes identiques
repliées, messages du design placés sur leur cycle) et `nvc_signal_drivers`
(process qui écrivent un signal, avec fichier et ligne, process en attente,
connexions de ports). Échantillonnage par front: la valeur juste avant le
front, ce que voient les registres.

Banc (`test/mcp/bench`): 5 designs, 8 bugs, vérité terrain calculée par
comparaison des ondes avec et sans le bug. Mesures avec le plus petit
modèle (3 essais par bug): voir §8.7.

### 8.7 Mesures du banc (modèle le plus petit, 3 essais par bug, 8 bugs)

| Configuration | Réussites | Appels moyens | Coût |
|---|---|---|---|
| Avec lecture des sources, outils d'avant la phase 1 | 24/24 | 11,0 | 0,127 $ |
| Sans sources, sans `find`/`measure`/`table` (ablation) | 23/24 | 12,2 | 0,166 $ |
| Sans sources, tous les outils | 24/24 | 12,5 | 0,194 $ |
| Sans sources, tous les outils et guidage | 21/24 | 12,1 | 0,167 $ |

Lecture honnête: **le banc ne départage pas les configurations**. Les écarts
(un à trois échecs sur 24) sont du bruit à cette taille d'échantillon, et le
petit modèle réussit déjà presque tout, même sans voir le source. Les trois
échecs de la dernière série sont des erreurs de définition de « première
valeur fausse » (le signal en aval au lieu du signal en amont, ou l'instant où
la sortie devient visiblement fausse au lieu de celui où le registre l'est),
pas des erreurs de calcul de temps. Ce que ces outils apportent est vérifié
par les tests unitaires (échantillonnage par front, délais en cycles,
conditions, repliement des cycles), pas par ce banc. Pour mesurer un gain il
faudrait des bugs plus durs (traces longues, nombreux signaux, causes à
plusieurs étapes), un modèle moyen en plus du petit, et bien plus d'essais.

### 8.8 Grand banc (19 bugs, 5 designs de plus, avec et sans sources)

Ajouts: un moteur DMA (le bug n'apparaît que quand la fin du transfert tombe
sur une fin de rafale), un arbitre à tourniquet (famine après quelques
dizaines de cycles), un récepteur UART (drapeau d'erreur jamais effacé, ou
parité sur 7 bits), un émetteur série lent avec CRC (CRC non remis à zéro,
ou mauvais polynôme, 100 horloges par bit), un compteur de ticks (retenue
perdue après 164 µs). Trois variantes silencieuses (le testbench ne vérifie
rien).

| Série | Réussites | Coût |
|---|---|---|
| petit modèle, sans sources, sans les outils temporels | 35/36 | 0,30 $ |
| petit modèle, sans sources, tous les outils | 34/36 | 0,29 $ |
| petit modèle, avec sources, tous les outils | 34/36 | 0,21 $ |
| modèle moyen, sans sources, sans les outils temporels | 18/18 | 1,03 $ |
| modèle moyen, sans sources, tous les outils | 18/18 | 1,10 $ |
| modèle moyen, avec sources, tous les outils | 14/18 | 0,94 $ |

Lecture: **pas de gain mesurable des outils temporels**; les deux modèles
réussissent presque tout sans eux. Les échecs se classent en trois familles:
(1) un écart d'une période d'horloge (163 825 ns au lieu de 163 815 ns): la
table montre la valeur avant le front, donc un registre écrit à un front
apparaît sur la ligne du front suivant; la description de l'outil le dit
désormais; (2) la confusion entre une erreur légitime et l'erreur
« jamais effacée » (`rx_noclear`: le petit modèle répond l'instant de la
première erreur de parité, qui est correcte); (3) un symptôme tardif donné
pour la cause (`dma_burst_end` avec sources: le petit modèle répond
l'instant où l'indice dépasse la fin, 2795 ns, au lieu de l'instant où la fin
de rafale est mal traitée, 875 ns). Avec les sources, le modèle moyen
répond plus vite (3 appels) mais se trompe plus (4 échecs sur 18, tous d'une
période d'horloge): il raisonne sur le code au lieu de mesurer. Ce banc ne
mesure donc que ce que les modèles font déjà bien; il reste à lui donner des
causes plus difficiles (plusieurs étapes, nombreux signaux, simulations
longues) ou des bugs réels.

### 8.9 Banc SoC: 23 fichiers, 10 bugs entre modules

`test/mcp/bench/soc`: un SoC de 21 fichiers de design (UART, FIFO, parseur
de commandes avec CRC et chien de garde, banc de registres, contrôleur
d'interruptions, timer, générateur de timing vidéo, motif, filtre 3 points,
gain/offset, statistiques, arbitre, constructeur de réponses) et un
testbench auto-vérifiant en 7 phases (modèle de référence pixel par pixel,
paquets de réponse vérifiés, ~416 µs simulées). Dix bugs, chacun dans un
autre fichier ou un câblage: place libre de FIFO mal calculée
(débordement seulement quand plusieurs réponses s'empilent), CRC non remis à
zéro après abandon d'un paquet, bord du filtre, extension de signe de
l'offset, effacement W1C qui efface tout, compteur de trames non remis à
zéro, enables permutés au niveau top, statistiques sans le dernier pixel,
taille d'image non relue au redémarrage, générique du chien de garde. La
cause apparaît entre 48 µs et 280 µs; le testbench s'en aperçoit souvent bien
plus tard et ailleurs. Juge: instant exact et chemin complet du signal
(`/dut/u_ctrl/u_parser/crc_clr`), plus fichier et ligne de la faute.

| Série (avec sources) | Instant et signal exacts | Bon fichier | Bonne ligne | Coût |
|---|---|---|---|---|
| petit modèle, sans outils temporels (2 essais) | 16/20 | 20/20 | 20/20 | 0,61 $ |
| petit modèle, tous les outils (2 essais) | 14/20 | 19/20 | 19/20 | 1,08 $ |
| modèle moyen, sans outils temporels | 10/10 | 10/10 | 10/10 | 1,85 $ |
| modèle moyen, tous les outils | 10/10 | 10/10 | 10/10 | ~2,0 $ |

Constats:
- Les deux modèles **localisent la faute dans le code presque toujours**
  (59/60 fichier et ligne). Avec les sources et une spécification en tête de
  chaque fichier, lire les 23 fichiers (environ 11 lectures par essai) suffit
  à trouver la plupart des bugs; la simulation sert à confirmer.
- Les échecs du petit modèle sont presque tous des **quasi-réussites**: le
  registre CRC un cycle après le signal `crc_clr` fautif, le signal de sortie
  du chien de garde 10 ns avant le compteur, ou le premier pixel faux au lieu
  du compteur de trame qui l'a causé. Un seul échec franc (un signal UART
  sans rapport).
- **Aucun gain mesurable des outils temporels**, et le petit modèle coûte
  plus cher avec (1,08 $ contre 0,61 $), surtout à cause d'un essai long.
  Ils sont peu utilisés: `nvc_wave_find` 18 fois et `nvc_wave_table` 10
  fois en 20 essais, `nvc_signal_drivers` jamais.
- Un défaut d'usage net: le petit modèle ouvre **3,7 simulations par essai**
  (74 `nvc_sim_open` pour 20 essais), parce qu'il faut démarrer
  l'enregistrement avant le run et qu'il découvre après coup quels signaux il
  lui fallait. Un enregistrement en mémoire de tous les signaux, démarré
  d'office à l'ouverture, éviterait ces relances.

### 8.10 Tri après mesure

`nvc_wave_measure` et `nvc_signal_drivers` ont été retirés: aucun appel en 30
essais sur le banc SoC et aucun gain mesuré (le code reste dans l'historique
git). `nvc_wave_find` et `nvc_wave_table` sont conservés. L'enregistrement
automatique à l'ouverture a été écarté: +3 à 6 % de temps sur un petit design
actif, tampon limité aux derniers changements et à 512 signaux, donc sans
intérêt pour une simulation longue.

### 8.11 Checkpoints (phase 1)

Réalisation de la phase 1 de `CHECKPOINT-PLAN.md`: retour en arrière par
instantanés, même design, en mémoire, sans format de fichier.

**Principe.** Un checkpoint est une copie gelée du worker, faite par `fork()`
entre deux tranches de simulation (`take_checkpoint`, `mcp-worker.c`). La copie
ne simule jamais: elle attend sur une socket privée dont l'autre bout est un
descripteur détenu par le superviseur (transmis par `SCM_RIGHTS` avec la
réponse). Fermer ce descripteur supprime le checkpoint; la mort du superviseur
les ferme tous. Pour restaurer, le superviseur crée un répertoire et une
socket de contrôle pour une nouvelle simulation et les envoie à la copie, qui
fait un second `fork()`: le petit-fils devient le worker de la nouvelle
simulation (nouveau groupe de processus, nouveau `output.log` copié jusqu'à
l'offset du checkpoint, état au repos au temps du checkpoint, événement `ready`
avec son pid). La copie reste gelée et peut être restaurée de nouveau.

**Outils.** `nvc_sim_checkpoint` (16 à la demande par simulation),
`nvc_sim_checkpoint_delete` (pas dans la liste du plan: sans lui l'erreur
« trop de checkpoints » n'aurait aucun moyen de libérer une place),
`nvc_sim_restore` (`checkpoint_id` ou `at`, donne une **nouvelle** simulation,
compte dans les 4 maximum), `nvc_sim_status` liste les checkpoints,
`nvc_sim_run` a `auto_checkpoint_s` (30 s par défaut pour un run de fond, 0 pour
aucun, nombre fractionnaire accepté). Au plus 8 checkpoints automatiques: au
neuvième on supprime celui dont les voisins sont les plus proches par rapport
à son âge, en gardant le plus ancien et le plus récent (écarts croissants avec
l'âge). Les checkpoints manuels ne sont jamais supprimés par ce tri.

**Fichiers du design** (`fileio.c`). Le registre des fichiers ouverts existait
déjà (table de poignées: `FILE *`, nom, mode). On y ajoute la position
logique (`ftello`), relevée dans le worker juste avant le `fork()`. Après
restauration, chaque fichier est rouvert par son chemin, sans création ni
troncature, `dup2` sur l'ancien descripteur, puis `fseeko` à la position
sauvée. Deux restaurations d'un même checkpoint lisent donc chacune leur
fichier à leur propre position. Un fichier qui a disparu fait échouer la
restauration avec son nom. **Fichiers en écriture:** la simulation restaurée
réécrit à partir de la position du checkpoint sans tronquer, donc elle écrase
ce que l'original a écrit après (et inversement si les deux tournent); en
mode ajout, elle écrit à la fin du fichier, après ce que l'original a pu
ajouter.

**Ondes.** Un enregistrement FST en cours appartient à l'original: la copie
l'abandonne sans l'écrire (`wave_dumper_abandon`, `wave.c`), la simulation
restaurée n'a pas d'enregistrement fichier (le `hint` du résultat le dit) et
l'enregistrement en mémoire continue.

**Refus.** Pas de checkpoint d'une simulation terminée ou finie (on restaure
un checkpoint antérieur, ce qui marche après une assertion fatale), ni
lorsqu'une bibliothèque externe est chargée (plugin VHPI `--load`, ou
bibliothèque d'un sous-programme `foreign` déjà appelé: `ffi_external_dll_loaded`).
Un sous-programme `foreign` pas encore appelé n'est pas détecté.

**Fin des processus.** Le worker d'origine meurt avec le superviseur
(`PR_SET_PDEATHSIG`), mais ce réglage est effacé dans l'enfant d'un `fork()`:
les copies ne l'ont pas. Elles meurent à la fin de fichier sur leur socket
privée (les copies sont bloquées en lecture dessus). Un worker restauré a pour
parent une copie qui peut disparaître avant lui (on ferme l'original, il reste
utilisable), donc `PDEATHSIG` ne conviendrait pas non plus; un fil de garde
surveille la fermeture de sa socket de contrôle, ce qui couvre un design qui
boucle sans jamais attendre (sans cela, mesuré: processus orphelin à 100 % de
CPU après un `kill -9` du serveur). `SIGCHLD` est ignoré dans le worker
dès le premier checkpoint pour que les copies soient récoltées sans
`waitpid`. Un worker restauré n'est pas un enfant du serveur: il est tué par
son pid, sans `waitpid`. Les copies dont le parent est déjà mort restent
zombies jusqu'à ce que `init` les récolte (quelques secondes dans le bac à
sable de test, aucune ressource tenue).

**Mesures de la phase 0** (build debug, interpréteur seul, 4 cœurs; avec
l'implémentation elle-même plutôt qu'un code jetable).

*Identité.* Pour chaque design: run d'un seul trait jusqu'à la fin contre run
jusqu'à T, checkpoint, restauration, run jusqu'à la fin. Comparés: valeurs de
tous les signaux lisibles, temps final, messages (sévérité, temps, delta,
texte), sortie renvoyée et `output.log`.

| Design | Signaux | Fin | T du checkpoint | Résultat |
|---|---|---|---|---|
| `counter` + `tb` (textio sur stdout, erreurs) | 7 | 1 us | 400 ns | identique |
| SoC `tb_soc` (23 fichiers) | 1133 | 415,865 us | 200 us | identique |
| `dma_tb` | 30 | 40 us | 17 us | identique |
| `crc_tx_tb` | 18 | 100 us | 47 us | identique |
| lecture de `stim.txt` et écriture de `result.txt` par textio | 3 | 3 us | 1 us | identique, fichier `result.txt` compris |

*Coût* (durée de l'appel d'outil vu du client, avec les échanges):

| Design | RSS du worker | Checkpoint | Restauration |
|---|---|---|---|
| SoC | 21 Mo | 0,4 à 0,8 ms | 0,7 à 0,9 ms |
| tableau de 10 millions d'entiers | 51 Mo | 0,3 à 0,6 ms | 0,5 ms |
| tableau de 100 millions d'entiers | 393 Mo | 0,6 à 0,8 ms (une fois 2,4) | 0,8 à 2,3 ms |

*Mémoire* (`/proc/<pid>/smaps_rollup`, un seul checkpoint). Juste après le
checkpoint, la copie a 0,1 Mo de mémoire privée sale et partage tout le reste
(Pss: la copie, l'original et le restauré comptent chacun un tiers). Après avoir
fait avancer l'original puis le restauré de 900 us sur le tableau de 10 millions
(touché sur 34,5 Mo): original 34,5 Mo privés, restauré 34,5 Mo privés,
**copie 34,5 Mo privés** (les anciennes versions des pages). Même chiffre pour
le tableau de 100 millions (393 Mo de RSS, 34,5 Mo réellement modifiés). Sur le
SoC: 0,4 Mo privés pour chacun des trois. Le coût d'un checkpoint est donc la
mémoire modifiée depuis, pas la taille du worker.

*Surcoût des checkpoints automatiques* (tableau de 100 millions, run de fond de
7,4 s): sans, 7,40 et 7,39 s; un par seconde, 7,96 et 7,40 s; cinq par seconde
(37 pris, 8 gardés), 7,68 et 7,68 s, soit environ +3,8 %. Au rythme par défaut
(un toutes les 30 s) c'est négligeable.

*Inventaire pour les phases suivantes* (`test/regress/testlist.txt`, 1519
entrées dont 1473 avec un fichier source du même nom, 1230 en VHDL et 243 en
Verilog; commentaires VHDL retirés, un seul fichier par test): déclaration
`file x : t` 36 tests, appel `file_open` 31 (36 pour l'un ou l'autre),
`use std.textio` ou `ieee.std_logic_textio` 49, types `access` 29, types
`protected` 44 (70 pour l'un ou l'autre; 6 mêlent fichiers et access ou
protégés). Options `vhpi` ou `vpi` dans la liste: 50 tests.

**Limites.** Non testé: macOS, build LLVM (les threads de compilation
asynchrone sont attendus avant le `fork()` avec `async_barrier()` et désactivés
dans les copies, mais rien n'a été exécuté avec LLVM), plugins VHPI, Windows
(le serveur MCP n'y existe pas). Fichiers en écriture: voir plus haut. Les
checkpoints disparaissent avec la simulation qui les a pris (fermeture,
expiration après 30 minutes sans appel) et avec le serveur. Au plus 8
checkpoints automatiques en attente de lecture par le superviseur: le worker en
cesse la prise si le superviseur n'a rien lu depuis huit (borne pour un run
sans surveillance); le superviseur lit les événements à chaque appel d'outil
et toutes les 30 s.

## Références

- Spécification MCP 2026-07-28, changements:
  <https://modelcontextprotocol.io/specification/2026-07-28/changelog>
- Outils: <https://modelcontextprotocol.io/specification/latest/server/tools>
- Transport stdio:
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/transports/stdio>
- Base du protocole (`_meta`, codes d'erreur, statelessness):
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/index>
- Versions et compatibilité:
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/versioning>
- `server/discover`:
  <https://modelcontextprotocol.io/specification/2026-07-28/server/discover>
- Annulation:
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/cancellation>
- Extension tâches: <https://modelcontextprotocol.io/docs/extensions/tasks>
- Sécurité:
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/security_best_practices>
- Anthropic, « Writing effective tools for agents »:
  <https://www.anthropic.com/engineering/writing-tools-for-agents>
- Serveurs MCP de simulation existants, exemple Verilator:
  <https://glama.ai/mcp/servers/@ssql2014/verilator-mcp/blob/dbddace3ea4c502f6f4b0731dfe074d96ca29c2f/README.md>
- Extension tâches, spécification normative:
  <https://github.com/modelcontextprotocol/ext-tasks/blob/main/specification/2026-07-28/tasks.md>
- Abonnements:
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/subscriptions>
- Progression:
  <https://modelcontextprotocol.io/specification/2026-07-28/basic/patterns/progress>
- Matrice de clients des extensions:
  <https://modelcontextprotocol.io/extensions/client-matrix>
- Claude Code, serveurs MCP (délais, tâches, arrière-plan):
  <https://code.claude.com/docs/en/mcp>
- Claude Code, canaux: <https://code.claude.com/docs/en/channels> et
  <https://code.claude.com/docs/en/channels-reference>
- Rapports sur des canaux qui ne réveillent pas une session inactive:
  <https://claudeissues.com/issue/38027-mcp-channel-notifications-should-wake-the-model-for-real-time-messaging>,
  <https://claudeissues.com/issue/63452-channel-notifications-notifications-claude-channel-buffered-instead-of-injected>
- Schémas d'agents asynchrones: <https://tianpan.co/blog/2026/03/07/async-agent-workflows-long-running-task-design>
  et <https://mastra.ai/docs/long-running-agents/background-tasks>
