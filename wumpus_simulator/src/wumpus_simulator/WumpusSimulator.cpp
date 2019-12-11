#include "wumpus_simulator/WumpusSimulator.h"

#include "model/Agent.h"
#include "model/GroundTile.h"
#include "model/Model.h"
#include "model/Movable.h"
#include "model/Wumpus.h"

#include <QMetaType>
#include <QUrl>
#include <QtNetwork/qnetworkproxy.h>
#include <QtWebKitWidgets/qwebframe.h>
#include <QtWebKitWidgets/qwebinspector.h>
#include <qdebug.h>
#include <qfiledialog.h>
#include <qgridlayout.h>

#include <pluginlib/class_list_macros.h>
#include <ros/master.h>

#include <memory>
#include <mutex>

namespace wumpus_simulator
{
WumpusSimulator::WumpusSimulator()
        : rqt_gui_cpp::Plugin()
        , widget_(0)
{
    setObjectName("WumpusSimulator");
    this->model = nullptr;
    turnIndex = 0;

    loadWorldSub = n.subscribe("/wumpus_simulator/LoadWorldRequest", 10, &WumpusSimulator::onLoadWorld, (WumpusSimulator*) this);
    spawnAgentSub = n.subscribe("/wumpus_simulator/SpawnAgentRequest", 10, &WumpusSimulator::onSpawnAgent, (WumpusSimulator*) this);
    spawnMultiAgentSub = n.subscribe("/wumpus_simulator/SpawnMultiAgentRequest", 10, &WumpusSimulator::onSpawnMultipleAgents, (WumpusSimulator*) this);
    actionSub = n.subscribe("/wumpus_simulator/ActionRequest", 10, &WumpusSimulator::onAction, (WumpusSimulator*) this);

    spawnAgentPub = n.advertise<wumpus_simulator::InitialPoseResponse>("/wumpus_simulator/SpawnAgentResponse", 10);
    spawnMultiAgentPub = n.advertise<wumpus_simulator::MultiInitialPoseResponse>("/wumpus_simulator/SpawnMultiAgentResponse", 10);
    actionPub = n.advertise<wumpus_simulator::ActionResponse>("/wumpus_simulator/ActionResponse", 10);

    spinner = new ros::AsyncSpinner(4);
    spinner->start();
    this->ready = false;
}

WumpusSimulator::~WumpusSimulator()
{
}

void WumpusSimulator::initPlugin(qt_gui_cpp::PluginContext& context)
{

    this->widget_ = new QWidget();
    this->widget_->setAttribute(Qt::WA_AlwaysShowToolTips, true);
    this->mainwindow.setupUi(this->widget_);

    if (context.serialNumber() > 1) {
        this->widget_->setWindowTitle(this->widget_->windowTitle() + " (" + QString::number(context.serialNumber()) + ")");
    }
    context.addWidget(this->widget_);

    // Allow to show a inspector in the web view, nice inline debugging of javascript
    this->mainwindow.webView->page()->settings()->setAttribute(QWebSettings::DeveloperExtrasEnabled, true);
    QWebInspector inspector;
    inspector.setPage(this->mainwindow.webView->page());
    inspector.setVisible(true);

    // Initialize web view
    this->connect(this->mainwindow.webView->page()->mainFrame(), SIGNAL(javaScriptWindowObjectCleared()), this, SLOT(addSimToJS()));
    this->mainwindow.webView->load(QUrl("qrc:///www/index.html"));
    this->connect(this, SIGNAL(modelChanged()), this, SLOT(callUpdatePlayground()));
    this->ready = false;
}

void WumpusSimulator::shutdownPlugin()
{
}

void WumpusSimulator::saveSettings(qt_gui_cpp::Settings& plugin_settings, qt_gui_cpp::Settings& instance_settings) const
{
}

void WumpusSimulator::restoreSettings(const qt_gui_cpp::Settings& plugin_settings, const qt_gui_cpp::Settings& instance_settings)
{
}

Model* WumpusSimulator::getModel()
{
    return this->model;
}

void WumpusSimulator::addSimToJS()
{
    // Windowscope => statisches Attribut => kann von überall in app aufgerufen werden
    this->mainwindow.webView->page()->mainFrame()->addToJavaScriptWindowObject("wumpus_simulator", this);
}

void WumpusSimulator::createWorld(bool arrow, int wumpus, int traps, int size)
{

    std::cout << "WumpusSimulator: Creating world with: arrow: " << (arrow ? "true" : "false") << " wumpus count: " << wumpus << " trap count: " << traps
              << " field size: " << size << std::endl;
    // Init the playground
    this->model = Model::get();
    this->model->init(arrow, wumpus, traps, size);
    updatePlayground();
    ready = true;
}

void WumpusSimulator::saveWorld()
{

    // Open save file dialog to select a pregenerated wumpus world
    QString filename = QFileDialog::getSaveFileName(
            this->widget_, tr("Save World"), QDir::currentPath(), tr("Wumpus World File (*.wwf)"), 0, QFileDialog::DontUseNativeDialog);

    if (!filename.isNull()) {
        if (!filename.endsWith(".wwf")) {
            filename += ".wwf";
        }

        // Check if the user selected a correct file
        if (!filename.isNull()) {

            // Create file
            QFile file(filename);

            if (!file.open(QIODevice::WriteOnly)) {
                std::cout << filename.toStdString() << std::endl;
                qWarning("Couldn't open save file.");
                return;
            }

            // Serialize the world as JSON
            if (this->model != nullptr) {
                auto worldJson = this->model->toJSON();

                // Write to file
                QJsonDocument saveDoc(worldJson);
                file.write(saveDoc.toJson());
            }
            file.close();
        }
    }
}

void WumpusSimulator::loadWorld(const std::string& worldPath)
{
    this->model = Model::get();
    this->turns.clear();
    // Open load file dialog to select a pregenerated wumpus world
    QString filename = QString::null;

    if (worldPath.empty()) {
        filename = QString::fromStdString(worldPath);
    } else {

        filename = QFileDialog::getOpenFileName(
                this->widget_, tr("Load World"), QDir::currentPath(), tr("Wumpus World File (*.wwf)"), 0, QFileDialog::DontUseNativeDialog);
    }

    // Check if the user selected a correct file
    if (!filename.isNull()) {
        // Open file
        QFile file(filename);
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning("Couldn't open save file.");
            return;
        }

        QByteArray saveData = file.readAll();
        QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
        this->model->fromJSON(loadDoc.object());
        this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(QString("setInitialValues(%1, %2, %3, %4);")
                                                                                  .arg(this->model->getWumpusCount())
                                                                                  .arg(this->model->getTrapCount())
                                                                                  .arg(this->model->getPlayGroundSize())
                                                                                  .arg(this->model->getAgentHasArrow()));
        this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(QString("drawPlayground();"));
        updatePlayground();
        ready = true;
    }
}

void WumpusSimulator::updatePlayground()
{
    QString clear = QString("clearTiles();");
    auto playGround = this->model->getPlayGround();
    this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(clear);
    for (int i = 0; i < this->model->getPlayGroundSize(); i++) {
        for (int j = 0; j < this->model->getPlayGroundSize(); j++) {
            QString f = QString("addDirtImage(%1,%2);").arg(i).arg(j);
            this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(f);
            if (playGround.at(i).at(j)->getStench()) {
                QString func = QString("addStenchImage(%1,%2);").arg(i).arg(j);
                this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
            }
            if (playGround.at(i).at(j)->getBreeze()) {
                QString func = QString("addBreezeImage(%1,%2);").arg(i).arg(j);
                this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
            }

            if (playGround.at(i).at(j)->getTrap()) {
                QString func = QString("addTrapImage(%1,%2);").arg(i).arg(j);
                this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
            }
            if (playGround.at(i).at(j)->getGold()) {
                QString func = QString("addGoldImage(%1,%2);").arg(i).arg(j);
                this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
            }
            if (playGround.at(i).at(j)->getStartpoint()) {
                QString func = QString("addEntryPoint(%1,%2);").arg(i).arg(j);
                this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
            }
            if (playGround.at(i).at(j)->hasMovable()) {
                for (auto mov : playGround.at(i).at(j)->getMovables()) {
//                    std::cout << "Iterating over Movables" << std::endl;
                    if (mov->getType().contains("wumpus")) {
                        QString func = QString("addWumpusImage(%1,%2);").arg(i).arg(j);
                        this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
                    }
                    if (mov->getType().contains("agent")) {
//                        std::cout << "Found agent - cast" << std::endl;
                        auto tmp = std::dynamic_pointer_cast<Agent>(mov);
//                        std::cout << "Cast done" << std::endl;

                        if (tmp == nullptr) {
                            continue;
                        }

                        if (mov->getId() % 2 == 0) {
                            QString func = QString("addAgent(%1,%2,%3,%4,%5);")
                                                   .arg(i)
                                                   .arg(j)
                                                   .arg(mov->getId())
                                                   .arg("\"female\"")
                                                   .arg(tmp->getHeading());
                            this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
                        } else {
                            QString func = QString("addAgent(%1,%2,%3,%4,%5);")
                                                   .arg(i)
                                                   .arg(j)
                                                   .arg(mov->getId())
                                                   .arg("\"male\"")
                                                   .arg(tmp->getHeading());
                            this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(func);
                        }
                    }
                }
            }
        }
    }
}

void WumpusSimulator::updatePlaygroundFromRequest()
{
    this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(QString("setInitialValues(%1, %2, %3, %4);")
                                                                              .arg(this->model->getWumpusCount())
                                                                              .arg(this->model->getTrapCount())
                                                                              .arg(this->model->getPlayGroundSize())
                                                                              .arg(this->model->getAgentHasArrow()));
    this->mainwindow.webView->page()->mainFrame()->evaluateJavaScript(QString("drawPlayground();"));
    this->updatePlayground();
}

void WumpusSimulator::onSpawnAgent(InitialPoseRequestPtr msg)
{
    if (!ready) {
        return;
    }
    if (msg->agentId > 0) {
        placeAgent(msg->agentId, this->model->getAgentHasArrow());
    } else if (msg->agentId < 0) {
        possessWumpus(msg->agentId);
    } else {
        std::cout << "WumpusSimulator: ID = 0 not supported!" << std::endl;
    }
}

void WumpusSimulator::onLoadWorld(LoadWorldRequestPtr msg)
{
    std::lock_guard<std::mutex> lock(this->loadWorldMtx);
    std::cout << "WumpusSimulator: onLoadWorld path: " << msg->worldPath << std::endl;
    if (std::find(this->loadedWorlds.begin(), this->loadedWorlds.end(), msg->worldPath) != this->loadedWorlds.end()) {
        std::cout << "World already loaded!" << std::endl;
        return;
    }
    this->loadedWorlds.push_back(msg->worldPath);
    this->model = Model::get();
    this->processedCombinations.clear();
    this->turns.clear();

    // Open load file dialog to select a pregenerated wumpus world

    // Check if the user selected a correct file
    if (!msg->worldPath.empty()) {
        // Open file
        QFile file(QString::fromStdString(msg->worldPath));
        if (!file.open(QIODevice::ReadOnly)) {
            qWarning("Couldn't open save file.");
            return;
        }

        QByteArray saveData = file.readAll();
        QJsonDocument loadDoc(QJsonDocument::fromJson(saveData));
        this->model->fromJSON(loadDoc.object());
        QMetaObject::invokeMethod(this, "updatePlaygroundFromRequest", Qt::ConnectionType::QueuedConnection);
        ready = true;
    }
}

/**
 * Performs checks whether the combination of
 * @param msg
 */
void WumpusSimulator::onSpawnMultipleAgents(MultiInitialPoseRequestPtr msg)
{
    std::lock_guard<std::mutex> lock(this->spawnAgentsMtx);
    std::cout << "Processing Spawn Request with Fields: ";
    for(auto pos : msg->requestedPositions) {
        std::cout << pos.x << ", " << pos.y << "; ";
    }
    std::cout << std::endl;
    std::vector<wumpus_simulator::InitialPoseResponse> responses;
    std::set<int> fields;
    for(auto pos : msg->requestedPositions) {
        fields.insert(pos.x * this->model->getPlayGroundSize() + pos.y);
    }

    if (std::find(this->processedCombinations.begin(), this->processedCombinations.end(), fields) != this->processedCombinations.end()) {
        std::cout << "Already tried this spawn combination! " << std::endl;
        return;
    }
    this->processedCombinations.push_back(fields);
    for (auto position : msg->requestedPositions) {
        auto tile = this->model->getPlayGround().at(position.x).at(position.y);
        if (!tile) {
            std::cout << "WumpusSimulator: Requested tile does not exist!" << std::endl;
            sendMultiSpawnUnsuccessfulMsg();
            return;
        }

        if (tile->getTrap() || tile->hasMovable() || tile->getGold() || tile->getStartpoint()) {
            std::cout << "WumpusSimulator: Tile occupied! " << position.x << ", " << position.y << std::endl;
            //FIXME hack
            this->processedCombinations.erase(std::find(this->processedCombinations.begin(), this->processedCombinations.end(), fields));
            sendMultiSpawnUnsuccessfulMsg();
            return;
        }
//        fields.insert(position.x * this->model->getPlayGroundSize() + position.y);
    }

    if (fields.size() != msg->requestedPositions.size()) {
        for (auto position : msg->requestedPositions) {
            std::cout << "Positions with duplicates: " << position.x << ", " << position.y << std::endl;
        }
        sendMultiSpawnUnsuccessfulMsg();
        return;
    }

    // spawning all agents possible
    this->turns.clear();
    for (auto position : msg->requestedPositions) {
        wumpus_simulator::InitialPoseResponse response;
        int heading = 0;
        if (position.heading == -1) {
            /* initialize random seed: */
            srand(time(NULL));
            heading = rand() % 4;
        } else {
            heading = position.heading;
        }
        std::cout << "WumpusSimulator: Spawning Agent!" << std::endl;
        spawnAgentOnTile(position.agentId, this->model->getAgentHasArrow(), position.x, position.y, response, heading,
                this->model->getPlayGround().at(position.x).at(position.y));
        // TODO review
    }

    emit modelChanged();
}

void WumpusSimulator::sendMultiSpawnUnsuccessfulMsg()
{

    wumpus_simulator::MultiInitialPoseResponse msg;
    msg.success = false;
    std::cout << "Sending unsuccessful msg" << std::endl;
    this->spawnMultiAgentPub.publish(msg);
};

void WumpusSimulator::callUpdatePlayground()
{
    updatePlayground();
}

void WumpusSimulator::onAction(ActionRequestPtr msg)
{
    if (!ready) {
        return;
    }
    if (turns.size() == 0) {
        return;
    }
    if (msg->agentId != this->turns.at(this->turnIndex)) {
        std::cout << "WumpusSimulator: agent is not allowed to move! It's agent's " << this->turns.at(this->turnIndex) << " turn!" << std::endl;
        return;
    }
    bool found = false;
    for (auto mov : this->model->movables) {
        if (msg->agentId == mov->getId()) {
            found = true;
            break;
        }
    }
    if (found) {
        if (msg->agentId > 0) {
            handleAction(msg);
        } else {
            handleWumpusAction(msg);
        }
    }
}

void WumpusSimulator::possessWumpus(int wumpusId)
{
    for (int i = 0; i < model->movables.size(); i++) {
        if (this->model->movables.at(i)->getId() == wumpusId) {
            std::cout << "WumpusSimulator: Wumpus with this id already possessed!" << std::endl;
            return;
        }
    }
    bool available = false;
    for (auto mov : this->model->movables) {
        if (mov->getId() == 0) {
            mov->setId(wumpusId);
            turns.push_back(wumpusId);
            available = true;
            break;
        }
    }
    if (!available) {
        std::cout << "WumpusSimulator: no Wumpus available!" << std::endl;
    }
}

void WumpusSimulator::placeAgent(int agentId, bool hasArrow)
{
    for (int i = 0; i < model->movables.size(); i++) {
        if (this->model->movables.at(i)->getId() == agentId) {
            std::cout << "WumpusSimulator: Agent with this id already placed!" << std::endl;
            return;
        }
    }
    /* initialize random seed: */
    srand(time(NULL));
    bool placed = false;
    InitialPoseResponse msg;
    int attempts = 0;
    while (!placed) {
        if (attempts > (pow(model->getPlayGroundSize(), 3))) {
            std::cout << "Abort! Cannot find empty tile to place agent on." << std::endl;
            break;
        }
        int randx = rand() % (this->model->getPlayGroundSize());
        int randy = rand() % (this->model->getPlayGroundSize());
        // random heading
        int randz = rand() % 4;

        auto tile = this->model->getPlayGround().at(randx).at(randy);
        if (!tile->getTrap() && !tile->hasMovable() && !tile->getGold() && !tile->getBreeze() && !tile->getStench() && !tile->getStartpoint()) {
            spawnAgentOnTile(agentId, hasArrow, randx, randy, msg, randz, tile);
            placed = true;
        }
        attempts++;
    }
    emit modelChanged();
}

void WumpusSimulator::placeAgentOnTile(int agentId, bool hasArrow, int x, int y)
{
    for (int i = 0; i < model->movables.size(); i++) {
        if (this->model->movables.at(i)->getId() == agentId) {
            std::cout << "WumpusSimulator: Agent with this id already placed!" << std::endl;
            return;
        }
    }
    InitialPoseResponse msg;

    // random heading
    int randz = rand() % 4;

    auto tile = this->model->getPlayGround().at(x).at(y);
    if (!tile) {
        std::cout << "WumpusSimulator: Requested tile does not exist!" << std::endl;
        return;
    }

    if (!tile->getTrap() && !tile->hasMovable() && !tile->getGold() && !tile->getStartpoint()) {
        spawnAgentOnTile(agentId, hasArrow, x, y, msg, randz, tile);
    }
    emit modelChanged();
}

void WumpusSimulator::spawnAgentOnTile(int agentId, bool hasArrow, int x, int y, InitialPoseResponse& msg, int z, std::shared_ptr<GroundTile>& tile)
{
    auto agent = std::make_shared<Agent>(model->getPlayGround().at(x).at(y));
    agent->setId(agentId);
    agent->setArrow(hasArrow);
    // agent->setHeading(WumpusEnums::heading::up);
    agent->setHeading(static_cast<WumpusEnums::heading>(z));
    model->getPlayGround().at(x).at(y)->addMovable(agent);
    model->movables.push_back(agent);
    tile->setStartAgentID(agentId);
    tile->setStartpoint(true);
    msg.x = x;
    msg.y = y;
    msg.agentId = agentId;
    msg.fieldSize = model->getPlayGroundSize();
    msg.hasArrow = hasArrow;
    msg.heading = agent->getHeading();
    spawnAgentPub.publish(msg);
    turns.push_back(agent->getId());
    if (turns.size() == 1) {
        ActionResponse msg2;
        msg2.x = agent->getTile()->getX();
        msg2.y = agent->getTile()->getY();
        msg2.agentId = agent->getId();
        msg2.heading = agent->getHeading();
        msg2.responses.push_back(WumpusEnums::yourTurn);
        handlePerception(msg2, agent->getTile());
        actionPub.publish(msg2);
    }
}

void WumpusSimulator::handleTurnRight(ActionRequestPtr msg)
{
    ActionResponse response;
    auto agent = this->model->getAgentByID(msg->agentId);
    auto tmp = (((agent->getHeading() - 1) + 4) % 4);
    agent->setHeading((WumpusEnums::heading)(tmp));
    response.agentId = agent->getId();
    response.x = agent->getTile()->getX();
    response.y = agent->getTile()->getY();
    response.heading = tmp;
    this->actionPub.publish(response);
    emit modelChanged();
}

void WumpusSimulator::handleTurnLeft(ActionRequestPtr msg)
{
    ActionResponse response;
    auto agent = this->model->getAgentByID(msg->agentId);
    auto tmp = ((agent->getHeading() + 1) % 4);
    agent->setHeading((WumpusEnums::heading)(tmp));
    response.agentId = agent->getId();
    response.x = agent->getTile()->getX();
    response.y = agent->getTile()->getY();
    response.heading = tmp;
    this->actionPub.publish(response);
    emit modelChanged();
}

void WumpusSimulator::handleShoot(ActionRequestPtr msg)
{
    ActionResponse response;
    auto agent = this->model->getAgentByID(msg->agentId);
    response.agentId = agent->getId();
    response.x = agent->getTile()->getX();
    response.y = agent->getTile()->getY();
    response.heading = agent->getHeading();
    if (agent->hasArrow()) {
        if (agent->getHeading() == WumpusEnums::heading::left) {
            handleShootLeft(response, agent);
        } else if (agent->getHeading() == WumpusEnums::heading::right) {
            handleShootRight(response, agent);
        } else if (agent->getHeading() == WumpusEnums::heading::up) {
            handleShootUp(response, agent);
        } else {
            handleShootDown(response, agent);
        }
        agent->setArrow(false);
        handlePerception(response, agent->getTile());
    } else {
        response.responses.push_back(WumpusEnums::responses::notAllowed);
        handlePerception(response, agent->getTile());
    }
    this->actionPub.publish(response);
    emit modelChanged();
}

void WumpusSimulator::handlePickUpGold(ActionRequestPtr msg)
{
    ActionResponse response;
    auto agent = this->model->getAgentByID(msg->agentId);
    response.agentId = agent->getId();
    response.x = agent->getTile()->getX();
    response.y = agent->getTile()->getY();
    response.heading = agent->getHeading();
    if (agent->getTile()->getGold()) {
        response.responses.push_back(WumpusEnums::responses::goldFound);
        agent->setHasGold(true);
    } else {
        response.responses.push_back(WumpusEnums::responses::notAllowed);
    }
    handlePerception(response, agent->getTile());
    this->actionPub.publish(response);
    emit modelChanged();
}

void WumpusSimulator::handleExit(ActionRequestPtr msg)
{
    ActionResponse response;
    auto agent = this->model->getAgentByID(msg->agentId);
    response.agentId = agent->getId();
    response.x = agent->getTile()->getX();
    response.y = agent->getTile()->getY();
    response.heading = agent->getHeading();
    if (agent->getHasGold() && agent->getTile()->getStartAgentID() == agent->getId()) {
        response.responses.push_back(WumpusEnums::responses::exited);
        this->turns.erase(std::find(this->turns.begin(), this->turns.end(), agent->getId()));
        this->model->exit(agent);
    } else {
        response.responses.push_back(WumpusEnums::responses::notAllowed);
    }
    this->actionPub.publish(response);
    emit modelChanged();
}

void WumpusSimulator::handleMove(ActionRequestPtr msg)
{
    ActionResponse response;
    auto agent = this->model->getAgentByID(msg->agentId);
    response.agentId = agent->getId();
    int x = agent->getTile()->getX();
    int y = agent->getTile()->getY();
    if ((x == 0 && agent->getHeading() == WumpusEnums::heading::up) ||
            (x == this->model->getPlayGroundSize() - 1 && agent->getHeading() == WumpusEnums::heading::down) ||
            (y == 0 && agent->getHeading() == WumpusEnums::heading::left) ||
            (y == this->model->getPlayGroundSize() - 1 && agent->getHeading() == WumpusEnums::heading::right)) {
        response.x = x;
        response.y = y;
        response.heading = agent->getHeading();
        response.responses.push_back(WumpusEnums::responses::bump);
    } else {
        if (agent->getHeading() == WumpusEnums::heading::up) {
            x -= 1;
        } else if (agent->getHeading() == WumpusEnums::heading::down) {
            x += 1;
        } else if (agent->getHeading() == WumpusEnums::heading::left) {
            y -= 1;
        } else {
            y += 1;
        }

        response.x = x;
        response.y = y;
        response.heading = agent->getHeading();

        if (this->model->getTile(x, y)->getTrap() || this->model->getTile(x, y)->hasWumpus()) {
            this->killAgent(agent);
//        } else if (this->model->getTile(x, y)->hasMovable() && !this->model->getTile(x, y)->hasWumpus()) {
//            response.x = agent->getTile()->getX();
//            response.y = agent->getTile()->getY();
//            response.responses.push_back(WumpusEnums::responses::otherAgent); //TODO removed so two agents can move to the same field
        } else {
            this->model->removeAgent(agent);
            agent->setTile(this->model->getTile(x, y));
            agent->getTile()->addMovable(agent);
        }
    }
    auto tmp = this->model->getTile(x, y);
    handlePerception(response, tmp);

    this->actionPub.publish(response);
    emit modelChanged();
}

void WumpusSimulator::handleWumpusAction(ActionRequestPtr msg)
{
    if (!(msg->action == WumpusEnums::heading::up || msg->action == WumpusEnums::heading::down || msg->action == WumpusEnums::heading::left ||
                msg->action == WumpusEnums::heading::right)) {

        std::cout << "WumpusSimulator: unknown Action received" << std::endl;
        return;
    }

    ActionResponse response;
    auto wumpus = this->model->getWumpusByID(msg->agentId);
    response.agentId = wumpus->getId();
    int x = wumpus->getTile()->getX();
    int y = wumpus->getTile()->getY();
    if ((x == 0 && msg->action == WumpusEnums::heading::up) || (x == this->model->getPlayGroundSize() - 1 && msg->action == WumpusEnums::heading::down) ||
            (y == 0 && msg->action == WumpusEnums::heading::left) ||
            (y == this->model->getPlayGroundSize() - 1 && msg->action == WumpusEnums::heading::right)) {
        response.x = x;
        response.y = y;
        response.heading = WumpusEnums::heading::down;
        response.responses.push_back(WumpusEnums::responses::bump);
    } else {
        if (msg->action == WumpusEnums::heading::up) {
            x -= 1;
        } else if (msg->action == WumpusEnums::heading::down) {
            x += 1;
        } else if (msg->action == WumpusEnums::heading::left) {
            y -= 1;
        } else {
            y += 1;
        }

        response.x = x;
        response.y = y;
        response.heading = WumpusEnums::heading::down;

        if (this->model->getTile(x, y)->hasWumpus()) {
            response.x = wumpus->getTile()->getX();
            response.y = wumpus->getTile()->getY();
            response.responses.push_back(WumpusEnums::responses::otherAgent);
        }

        if (this->model->getTile(x, y)->hasMovable() && !this->model->getTile(x, y)->hasWumpus()) {
            // auto tmp = std::dynamic_pointer_cast<Agent>(this->model->getTile(x, y)->getMovables());
            for (auto tmp : this->model->getTile(x, y)->getAgents()) {
                this->killAgent(tmp);
            }
            response.responses.push_back(WumpusEnums::responses::killedAgent);
        }

        this->model->removeWumpus(wumpus);
        wumpus->setTile(this->model->getTile(x, y));
        wumpus->getTile()->addMovable(wumpus);
        for (auto mov : this->model->movables) {
            if (mov->getId() <= 0) {
                this->model->setStench(mov->getTile()->getX(), mov->getTile()->getY());
            }
        }
    }
    this->actionPub.publish(response);
    handleNextTurn();
    emit modelChanged();
}

void WumpusSimulator::handleAction(ActionRequestPtr msg)
{

    switch (msg->action) {
    case WumpusEnums::actions::move: {
        handleMove(msg);
        break;
    }
    case WumpusEnums::actions::leave: {
        handleExit(msg);
        break;
    }
    case WumpusEnums::actions::pickUpGold: {
        handlePickUpGold(msg);
        break;
    }
    case WumpusEnums::actions::shoot: {
        handleShoot(msg);
        break;
    }
    case WumpusEnums::actions::turnLeft: {
        handleTurnLeft(msg);
        break;
    }
    case WumpusEnums::actions::turnRight: {
        handleTurnRight(msg);
        break;
    }

    default:
        std::cout << "WumpusSimulator: unknown Action received" << std::endl;
        break;
    }
    handleNextTurn();
}

void WumpusSimulator::handleNextTurn()
{
    if (this->turns.size() == 0) {
        return;
    }
    getNext();
    ActionResponse response;
    auto id = this->turns.at(turnIndex);
    response.agentId = id;
    if (id > 0) {
        auto agent = this->model->getAgentByID(id);
        response.heading = agent->getHeading();
        auto tmp = agent->getTile();
        handlePerception(response, tmp);
        response.x = tmp->getX();
        response.y = tmp->getY();
    } else {
        auto wumpus = this->model->getWumpusByID(id);
        auto tmp = wumpus->getTile();
        response.x = tmp->getX();
        response.y = tmp->getY();
    }
    response.responses.push_back(WumpusEnums::responses::yourTurn);
    this->actionPub.publish(response);
}

void WumpusSimulator::handlePerception(ActionResponse& msg, std::shared_ptr<GroundTile> tile)
{
    if (tile->getGold()) {
        msg.responses.push_back(WumpusEnums::responses::shiny);
    }
    if (tile->getBreeze()) {
        msg.responses.push_back(WumpusEnums::responses::drafty);
    }
    if (tile->getStench()) {
        msg.responses.push_back(WumpusEnums::responses::stinky);
    }
}

void WumpusSimulator::handleShootLeft(ActionResponse& msg, std::shared_ptr<Agent> agent)
{
    bool wumpusDead = false;
    if (agent->getTile()->getY() == 0) {
        msg.responses.push_back(WumpusEnums::responses::silence);
    } else {
        for (int i = agent->getTile()->getY() - 1; i >= 0; i--) {
            if (this->getModel()->getPlayGround().at(agent->getTile()->getX()).at(i)->hasWumpus()) {
                wumpusDead = true;
                auto wumpi = this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->getWumpi();
                for (auto w : wumpi) {
                    killWumpus(w);
                }
            }
        }
        if (wumpusDead) {
            msg.responses.push_back(WumpusEnums::responses::scream);
        } else {
            msg.responses.push_back(WumpusEnums::responses::silence);
        }
    }
}

void WumpusSimulator::handleShootRight(ActionResponse& msg, std::shared_ptr<Agent> agent)
{
    bool wumpusDead = false;
    if (agent->getTile()->getY() == this->model->getPlayGroundSize() - 1) {
        msg.responses.push_back(WumpusEnums::responses::silence);
    } else {
        for (int i = agent->getTile()->getY() + 1; i <= this->model->getPlayGroundSize() - 1; i++) {
            if (this->getModel()->getPlayGround().at(agent->getTile()->getX()).at(i)->hasWumpus()) {
                wumpusDead = true;
                auto wumpi = this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->getWumpi();
                for (auto w : wumpi) {
                    killWumpus(w);
                }
            }
        }
        if (wumpusDead) {
            msg.responses.push_back(WumpusEnums::responses::scream);
        } else {
            msg.responses.push_back(WumpusEnums::responses::silence);
        }
    }
}

void WumpusSimulator::handleShootUp(ActionResponse& msg, std::shared_ptr<Agent> agent)
{
    bool wumpusDead = false;
    if (agent->getTile()->getX() == 0) {
        msg.responses.push_back(WumpusEnums::responses::silence);
    } else {
        for (int i = agent->getTile()->getX() - 1; i >= 0; i--) {
            if (this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->hasWumpus()) {
                wumpusDead = true;
                auto wumpi = this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->getWumpi();
                for (auto w : wumpi) {
                    killWumpus(w);
                }
            }
        }
        if (wumpusDead) {
            msg.responses.push_back(WumpusEnums::responses::scream);
        } else {
            msg.responses.push_back(WumpusEnums::responses::silence);
        }
    }
}

void WumpusSimulator::handleShootDown(ActionResponse& msg, std::shared_ptr<Agent> agent)
{
    bool wumpusDead = false;
    if (agent->getTile()->getX() == this->model->getPlayGroundSize() - 1) {
        msg.responses.push_back(WumpusEnums::responses::silence);
    } else {
        for (int i = agent->getTile()->getX() + 1; i <= this->model->getPlayGroundSize() - 1; i++) {
            if (this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->hasWumpus()) {
                wumpusDead = true;
                //                auto tmp = std::dynamic_pointer_cast<Wumpus>(
                //                        this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->getMovables());
                auto wumpi = this->getModel()->getPlayGround().at(i).at(agent->getTile()->getY())->getWumpi();
                for (auto w : wumpi) {
                    killWumpus(w);
                }
            }
        }
        if (wumpusDead) {
            msg.responses.push_back(WumpusEnums::responses::scream);
        } else {
            msg.responses.push_back(WumpusEnums::responses::silence);
        }
    }
}

void WumpusSimulator::killWumpus(std::shared_ptr<Wumpus> wumpus)
{
    if (wumpus->getId() != 0) {
        ActionResponse response2;
        response2.agentId = wumpus->getId();
        response2.responses.push_back(WumpusEnums::responses::dead);
        this->actionPub.publish(response2);
        this->turns.erase(std::find(this->turns.begin(), this->turns.end(), wumpus->getId()));
    }
    this->getModel()->removeWumpus(wumpus);
    this->model->movables.erase(remove(this->model->movables.begin(), this->model->movables.end(), wumpus), this->model->movables.end());
    for (auto mov : this->model->movables) {
        if (mov->getId() <= 0) {
            this->model->setStench(mov->getTile()->getX(), mov->getTile()->getY());
        }
    }
    emit modelChanged();
}

void WumpusSimulator::killAgent(std::shared_ptr<Agent> agent)
{
    this->turns.erase(std::find(this->turns.begin(), this->turns.end(), agent->getId()));
    ActionResponse response2;
    response2.agentId = agent->getId();
    response2.responses.push_back(WumpusEnums::responses::dead);
    this->actionPub.publish(response2);
    this->model->exit(agent);
    emit modelChanged();
}

void WumpusSimulator::getNext()
{
    this->turnIndex++;
    if (this->turnIndex > this->turns.size()) {
        this->turnIndex = this->turns.size();
    }
    this->turnIndex = turnIndex % this->turns.size();
}
}

PLUGINLIB_EXPORT_CLASS(wumpus_simulator::WumpusSimulator, rqt_gui_cpp::Plugin)
