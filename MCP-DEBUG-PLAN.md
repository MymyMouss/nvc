# Plan: donner à un agent les moyens de diagnostiquer par les ondes

Statut: **phases 0 à 3, 5 (étape 1), 6 et 7 réalisées** (voir
`MCP-ARCHITECTURE.md` §8.6 et §8.7). Non faites: phase 4 (`nvc_wave_diff`) et
l'étape 2 de la phase 5 (cône d'influence), faute de besoin démontré par le
banc; le banc actuel est trop facile pour trancher. Il complète `MCP-ARCHITECTURE.md` (§8.5 décrit
l'existant).

## 1. Le problème

Le serveur MCP sait aujourd'hui enregistrer des ondes et les relire
(`nvc_wave_get`, `nvc_wave_find`, `nvc_trace_get`). Ces outils rendent des
listes de changements `[instant, valeur]`. L'agent doit ensuite en déduire
lui-même l'ordre des événements, les délais et les corrélations.

Or c'est précisément ce que les modèles font mal. WaveformQA (2026, 360
questions sur des traces de cœurs RISC-V) mesure:

| Type de question | Erreurs |
|---|---|
| Lire une valeur, compter, calculer | faibles |
| « Lequel change en premier » | environ 95 % |
| Corrélation entre signaux | environ 69 % |
| « Combien de cycles après tel événement » | environ 65 % |

La précision baisse aussi de 8 à 12 % entre 5 000 et 30 000 transitions,
même quand tout tient dans le contexte. Les erreurs sont surtout de mauvais
ordre et de mauvais signal, rarement de mauvaise valeur.

Les systèmes qui marchent (VeriTrace, BTTF, VerilogCoder) ont le même
schéma: le modèle ne lit jamais les ondes brutes. Il pose des questions
ciblées à un outil qui fait le calcul temporel, en partant de l'instant de
l'erreur et en remontant vers la cause.

## 2. Principes retenus

- **P1. L'outil calcule le temps, l'agent interprète.** Toute question
  d'ordre, de délai, de durée ou de comptage doit avoir une réponse directe
  d'un outil, jamais une liste à dépouiller.
- **P2. Vue par cycle d'horloge.** Une table « une ligne par front
  d'horloge » se lit comme un chronogramme. Une liste de changements
  asynchrones ne se lit pas.
- **P3. Partir de l'erreur et remonter.** L'outil donne le point de départ
  (instant et signaux de l'assertion) puis les moyens de remonter d'un
  signal à ce qui le pilote.
- **P4. Réponses petites, avec la preuve.** Chaque réponse tient en
  quelques centaines de tokens, donne la conclusion et les instants et
  valeurs qui la justifient.
- **P5. Mesurer avant et après.** Un outil n'est gardé que s'il améliore le
  taux de réussite ou le coût sur un banc de bugs réalistes.
- **P6. Peu d'outils.** Le serveur en a déjà 23. On fusionne avant
  d'ajouter.

## 3. Phases

Les tailles sont relatives: S = quelques heures de travail d'agent, M = une
session, L = plusieurs sessions.

### Phase 0. Banc de mesure (M), avant tout nouvel outil

Les six scénarios actuels (un compteur, une assertion) sont trop faciles:
ils passent tous avec le plus petit modèle et ne départagent rien.

**0.1 Designs.** Cinq petits designs VHDL réalistes (100 à 300 lignes
chacun), avec un testbench:

| Design | Pourquoi |
|---|---|
| Contrôleur à automate d'états | bugs de transition |
| Handshake valid/ready avec FIFO | données perdues ou dupliquées |
| UART émetteur et récepteur | liaison série lente, événements rares |
| Compteurs ligne/trame vidéo avec synchros | décalages d'un pixel, simulation longue |
| Pipeline à trois étages | décalage d'un cycle |

**0.2 Bugs injectés.** Dix à douze variantes, une par bug:

1. automate qui saute un état;
2. transition manquante (blocage dans un état);
3. donnée perdue quand `ready` retombe;
4. donnée dupliquée dans la FIFO;
5. ligne vidéo trop longue d'un pixel;
6. sortie de pipeline en avance d'un cycle;
7. registre non remis à zéro;
8. troncature d'un compteur trop étroit;
9. course entre deux process (dépend de l'ordre des deltas);
10. bit de parité UART faux, visible seulement au N-ième octet;
11. bug qui n'apparaît qu'après plus de 10 ms simulées (exerce le run de
    fond);
12. bug silencieux: aucune assertion, seule la sortie est fausse.

Pour chacun la vérité terrain est connue: fichier et ligne fautifs, signal
en cause, instant de la première divergence.

**0.3 Deux types de tâches.**

- *Diagnostic*: sans accès aux fichiers en écriture, l'agent doit rendre
  `signal`, `instant` et `cause`. Vérifié par programme.
- *Réparation*: avec lecture et édition des sources, l'agent corrige. On
  vérifie en relançant le testbench sain, jamais sur la parole de l'agent.

**0.4 Micro-questions temporelles.** Vingt questions à vérité calculée par
programme, calquées sur WaveformQA (ordre, cycles après déclencheur, durée
d'impulsion, comptage, stabilité). Elles mesurent la brique seule, et
servent ensuite de tests unitaires des opérateurs, sans modèle.

**0.5 Mesures.** Taux de réussite sur trois essais par scénario, nombre
d'appels, tokens, coût, durée. Deux modèles: le plus petit (comme
aujourd'hui) et un modèle intermédiaire. Ordre de grandeur du coût: un
scénario actuel coûte environ 0,005 $ avec le petit modèle; le banc complet
devrait rester à quelques dollars par passe, à confirmer à la première.

**Livrable.** Un tableau de référence avec les outils actuels, et la liste
des échecs observés en lisant les transcriptions. **Cette liste fixe
l'ordre et le contenu des phases 2 à 5**: on ne construit que ce qui répond
à un échec constaté.

### Phase 1. Une seule notion d'enregistrement (M)

Aujourd'hui il y a deux mondes: la trace en mémoire (lisible en direct) et
le fichier FST (lisible une fois fermé), avec des outils, des formats de
réponse et des noms de paramètres différents (`to` contre `until`, `bin`
contre `binary`).

- **Couche interne commune**: un itérateur de changements `(instant,
  signal, valeur)` au-dessus d'un FST fermé ou du tampon en mémoire. Tous
  les outils de requête s'appuient dessus.
- **`nvc_wave_start` garde aussi un tampon en mémoire** sur les signaux
  enregistrés (dans les limites actuelles: 512 signaux, 64 Mio). Les
  requêtes prennent `sim_id` (direct, pendant le run) ou `file` (fichier
  terminé).
- **Fusion des outils `nvc_trace_*` dans `nvc_wave_*`**: `nvc_wave_start`
  reçoit `to: "memory" | "file" | "both"`. Trois outils en moins.
- **Harmonisation**: mêmes noms de paramètres, mêmes chemins, mêmes radix,
  énumérations rendues par leur nom (à vérifier pour le FST: voir risques).
- **Requêtes sur fichier dans un processus enfant** avec budget de temps
  et annulation. Aujourd'hui le lecteur FST tourne dans le front-end: un
  gros fichier bloque le serveur et un fichier corrompu peut le faire
  tomber, ce qui contredit la décision D1 de l'architecture.

### Phase 2. Vue par cycle: `nvc_wave_table` (M)

Une table échantillonnée sur une horloge:

```
cycle  time     /dut/state  /dut/valid  /dut/ready  /dut/cnt
  12   125 ns   IDLE        0           1           3
  13   135 ns   LOAD        1           1           3
  14   145 ns   SEND        1           0           4      <- error: "data lost"
  15…22 (8 cycles identiques)
```

- Paramètres: source, `clock` et front, jusqu'à 16 signaux, fenêtre
  (`from`/`until`, ou `around` un instant plus ou moins N cycles), radix,
  200 lignes au plus.
- Les suites de lignes identiques sont repliées (« 8 cycles identiques »).
- Les messages `report`/`assert` de la simulation sont placés sur le cycle
  où ils sont survenus.
- Sans horloge: lignes groupées par instant de changement.
- **Sémantique d'échantillonnage**: la valeur juste avant le front, donc
  ce que voient les bascules (comme en SVA). C'est le choix qui évite les
  faux décalages d'un cycle; il sera écrit dans la description de l'outil.

### Phase 3. Opérateurs temporels (L)

Deux outils, plutôt qu'un par opération.

**`nvc_wave_find` étendu: quand une condition est-elle vraie ?**

- Une condition sur plusieurs signaux, en syntaxe proche de VHDL:
  `/dut/valid = '1' and /dut/ready = '0'`, `/dut/cnt > 100`,
  `rose(/dut/start)`.
- Options: `clock` (évaluation par cycle), `for_at_least` (N cycles ou une
  durée), fenêtre, nombre maximal de résultats.
- Réponse: les intervalles `[début, fin, durée, cycles]` et leur nombre
  total.
- Couvre: premier instant où X après T, stabilité, durée d'une impulsion,
  nombre d'occurrences, « bloqué pendant plus de N cycles ».

**`nvc_wave_measure`: relation entre deux événements.**

- Deux événements A et B, chacun une condition qui devient vraie.
- Réponse: pour chaque A le B suivant, le délai en temps et en cycles;
  minimum, maximum, moyenne; les A sans B; et un verdict d'ordre explicite
  (« A précède B de 3 cycles à 145 ns »).
- Couvre: lequel change en premier, cycles après déclencheur, latence d'un
  handshake, période et gigue.

**Langage de conditions**, volontairement petit: chemins de signaux,
littéraux (bit, vecteur binaire, hexadécimal, décimal, nom d'énumération),
`= /= < <= > >=`, `and or not`, parenthèses, `rose` `fell` `changed`
`stable`. Huit signaux au plus par condition. Les erreurs de syntaxe
indiquent la position et un exemple correct.

### Phase 4. Comparaison: `nvc_wave_diff` (M), si le banc le justifie

Première divergence entre deux enregistrements (avant et après une
correction, graine qui passe contre graine qui échoue), ou entre deux
signaux (attendu contre obtenu, avec un décalage en cycles). Réponse:
instant de la première différence, signaux concernés, table autour.

### Phase 5. Remonter aux causes: `nvc_signal_drivers` (M, puis L)

Sur une simulation vivante, pour un signal:

- **Étape 1 (M), disponible dans le modèle d'exécution**: les process qui
  le pilotent (nom, fichier et ligne), les connexions de port vers le haut
  de la hiérarchie, si le signal est forcé; les process qui y sont
  sensibles (les « lecteurs »).
- **Étape 2 (L)**: les signaux que lit chaque process pilote, par parcours
  de son corps dans le design élaboré. L'agent peut alors remonter le cône
  d'influence pas à pas, de l'erreur vers la cause.
- **Étape 3 (exploratoire)**: quel process a écrit le signal en dernier et
  quand. Faisabilité à vérifier avant de s'engager.

### Phase 6. Guidage de l'agent (S)

- **Point de départ donné par l'outil**: quand `nvc_sim_run` s'arrête sur
  un message, la réponse nomme les signaux du process qui l'a émis.
- **Méthode en cinq lignes** dans les instructions du serveur: aller à la
  première erreur; table autour sur les signaux concernés; confirmer
  l'hypothèse par `find`/`measure`; remonter par `drivers`; lire la source
  à la ligne indiquée.
- Indications `hint` dans les réponses, comme aujourd'hui.

### Phase 7. Re-mesure et tri (M)

Rejouer le banc de la phase 0. Garder ce qui améliore la réussite ou le
coût, retirer le reste. Résultats consignés dans `MCP-ARCHITECTURE.md`.
Tests unitaires de chaque opérateur à partir des micro-questions, sans
modèle. Régression complète.

## 4. Ordre proposé

0 → 1 → 2 → 3 → 6 → 7, puis 5 et 4 selon ce que montre le banc.

La phase 0 peut modifier cet ordre: si les échecs viennent surtout de
l'impossibilité de remonter aux causes, la phase 5 passe devant la 3.

## 5. Hors périmètre

- **Images de chronogrammes** données au modèle: non évalué dans l'état de
  l'art, coûteux, et redondant avec la table par cycle.
- **Base SQL** (approche BTTF): dépendance supplémentaire dans nvc, et les
  questions d'ordre restent pénibles en SQL.
- **Assertions PSL évaluées après coup**: les conditions de la phase 3
  couvrent le besoin à moindre coût.
- **Reprise sur checkpoint** (`PLAN.md` §1): c'est la vraie réponse à « j'ai
  oublié d'enregistrer ce signal », mais c'est un chantier séparé.

## 6. Risques

- **Dérive du langage de conditions.** Parade: grammaire figée par les
  micro-questions; pas de fonctions au-delà des quatre listées.
- **Énumérations dans le FST.** Si les états d'automate n'y sont pas
  stockés par leur nom, il faudra une table de correspondance écrite à
  l'enregistrement. À vérifier au début de la phase 1.
- **Sémantique d'échantillonnage.** Une erreur ici crée exactement les
  décalages d'un cycle qu'on veut aider à trouver. Parade: tests sur des
  cas à vérité connue avant tout usage par un agent.
- **Banc trop facile ou trop artificiel.** Des bugs réels venant de vos
  projets vaudraient mieux que des bugs injectés (voir décisions).
- **Coût des fichiers volumineux.** Les requêtes parcourent la fenêtre
  demandée; le processus enfant et son budget de temps bornent le pire cas.

## 7. Décisions à prendre avant de commencer

1. **Fusionner `nvc_trace_*` dans `nvc_wave_*`** (recommandé: oui). Moins
   d'outils et une seule façon d'enregistrer; en échange les noms actuels
   disparaissent, ce qui est sans conséquence tant qu'il n'y a pas
   d'utilisateurs.
2. **Syntaxe des conditions proche de VHDL** (recommandé) plutôt qu'une
   structure JSON. Un agent qui débogue du VHDL l'écrit naturellement; en
   échange il faut un petit analyseur et de bons messages d'erreur.
3. **Contenu du banc**: les cinq designs proposés conviennent-ils, et
   avez-vous des bugs réels (ou leur description) à y ajouter ?
4. **Modèles et budget de mesure**: petit modèle plus un intermédiaire,
   trois essais, quelques dollars par passe.

## Références

- WaveformQA: https://arxiv.org/pdf/2607.20638
- VeriTrace: https://arxiv.org/html/2608.02878
- BTTF (Columbia, Google):
  https://semiengineering.com/agentic-ai-automates-rtl-waveform-analysis-and-debugging-columbia-google/
- FVDebug (NVIDIA): https://arxiv.org/pdf/2510.15906
- A Survey on LLM-Integrated Hardware Design Verification:
  https://arxiv.org/html/2610.10580v1
