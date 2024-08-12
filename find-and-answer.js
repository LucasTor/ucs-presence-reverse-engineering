import Axios from "axios";
import chalk from "chalk";
import moment from "moment";
import fs from "fs";

const api = Axios.create({
  baseURL: "https://sou.ucs.br/api/v1/",
});

const { username, password, webhookUrl } = JSON.parse(
  fs.readFileSync("./credentials.json")
);

const debug = (msg) => Axios.post(webhookUrl, { content: msg });
const success = (msg) =>
  Axios.post(webhookUrl, { embeds: [{ title: msg, color: "10747768" }] });

const run = async () => {
  try {
    console.log(chalk.cyan("Fetching token..."));
    await debug("Starting script, fetching token...");

    const token = await api.post(
      "https://auth.ucs.br/auth-token/api-token-auth/",
      {
        username,
        password,
      }
    );

    console.log(chalk.cyan("Fetching classes..."));
    await debug("Fetching classes...");

    const classes = await api.get("/ambientes/segmentos/graduacao/ambientes/", {
      headers: {
        Authorization: `Token ${token.data.token}`,
      },
    });

    // console.log(JSON.stringify(classes.data, null, 2));

    console.log(chalk.cyan("Fetching classes details..."));
    await debug("Fetching classes details...");
    const classesDetails = await Promise.all(
      classes.data
        .flatMap((c) => c.itens)
        .map((c) =>
          api
            .get(
              `/ambientes/segmentos/graduacao/ambientes/${c.url}/ferramentas/registro-frequencia/`,
              {
                headers: {
                  Authorization: `Token ${token.data.token}`,
                },
              }
            )
            .then(({ data }) => ({
              ...data,
              class: c,
            }))
        )
    );

    console.log(chalk.cyan("Trying to find today's class..."));
    await debug("Trying to find todays class...");

    // classesDetails[0].dados.encontro_hoje = {
    //   sequencia: 1,
    //   horas: 4.0,
    //   data: "2024-03-07T00:00:00",
    //   faltas: 0,
    //   chamada_app_hoje: false,
    // };

    // console.log(classesDetails.map((c) => ({ ...c.dados, nome: c.class.nome })));

    const todaysClass = classesDetails.find((c) => c.dados.encontro_hoje);

    if (!todaysClass) {
      console.log(chalk.redBright("Failed to find today's class, exiting."));
      await debug("Failed to find todays class, exiting...");
      process.exit();
    }

    console.log(chalk.green("Found todays class!"));
    await debug("Found todays class!");
    const todaysClassUrl = todaysClass.class.url;
    await debug(todaysClassUrl);
    console.log(chalk.green(todaysClassUrl));

    // console.log(JSON.stringify(todaysClass.dados.encontro_hoje, null, 2));

    const checkAndAnswerAttendence = async () => {
      console.log(
        chalk.cyan(
          `Checking attendence registration availability at ${moment().format(
            "HH:mm:ss"
          )}...`
        )
      );
      await debug(
        `Checking attendence registration availability at ${moment().format(
          "HH:mm:ss"
        )}...`
      );

      const updatedTodaysClass = await api
        .get(
          `/ambientes/segmentos/graduacao/ambientes/${todaysClassUrl}/ferramentas/registro-frequencia/`,
          {
            headers: {
              Authorization: `Token ${token.data.token}`,
            },
          }
        )
        .then(({ data }) => data);

      if (
        updatedTodaysClass.dados.encontro_hoje?.chamada_app_hoje
          ?.liberada_para_membros
      ) {
        console.log(
          chalk.cyan("Attendence registration available!! Responding now...")
        );

        await debug("Attendence registration available!! Responding now...");

        await api.post(
          `/ambientes/segmentos/graduacao/ambientes/${todaysClassUrl}/ferramentas/registro-frequencia/`,
          {
            funcao: "atualizar_registro_frequencia_via_membro",
            parametros: {
              sequencia: todaysClass.dados.encontro_hoje.sequencia,
            },
          },
          {
            headers: {
              Authorization: `Token ${token.data.token}`,
            },
          }
        );
        console.log(
          chalk.greenBright("Sucess responding to attendence registration!")
        );
        await success("Sucess responding to attendence registration!");
        process.exit();
      } else
        console.log(
          chalk.yellowBright("Attendence registration not available.")
        );
      await debug("Attendence registration not available.");
    };

    await checkAndAnswerAttendence();
    setInterval(() => checkAndAnswerAttendence(), 30000);
  } catch (e) {
    await debug(e.stack);
    console.error(e);
    process.exit(1);
  }
};

setTimeout(async () => {
  await debug("Timeout reached, exiting.");
  process.exit();
}, 1000 * 60 * 60 * 3);

run();
