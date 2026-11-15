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
| `nvc_trace_start` | Enregistrer les changements de signaux en mémoire (anneau borné) | non | non | oui |
| `nvc_trace_get` | Lire les changements sur une fenêtre, paginé | oui | non | oui |
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

## 5. Points à valider et limites de cette étude

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
